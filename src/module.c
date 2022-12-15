// This file is a part of Julia. License is MIT: https://julialang.org/license

/*
  modules and top-level bindings
*/
#include "julia.h"
#include "julia_internal.h"
#include "julia_assert.h"

#ifdef __cplusplus
extern "C" {
#endif

JL_DLLEXPORT jl_module_t *jl_new_module_(jl_sym_t *name, jl_module_t *parent, uint8_t default_names)
{
    jl_task_t *ct = jl_current_task;
    const jl_uuid_t uuid_zero = {0, 0};
    jl_module_t *m = (jl_module_t*)jl_gc_alloc(ct->ptls, sizeof(jl_module_t),
                                               jl_module_type);
    assert(jl_is_symbol(name));
    m->name = name;
    m->parent = parent;
    m->istopmod = 0;
    m->uuid = uuid_zero;
    static unsigned int mcounter; // simple counter backup, in case hrtime is not incrementing
    m->build_id.lo = jl_hrtime() + (++mcounter);
    if (!m->build_id.lo)
        m->build_id.lo++; // build id 0 is invalid
    m->build_id.hi = ~(uint64_t)0;
    m->primary_world = 0;
    jl_atomic_store_relaxed(&m->counter, 1);
    m->nospecialize = 0;
    m->optlevel = -1;
    m->compile = -1;
    m->infer = -1;
    m->max_methods = -1;
    m->hash = parent == NULL ? bitmix(name->hash, jl_module_type->hash) :
        bitmix(name->hash, parent->hash);
    JL_MUTEX_INIT(&m->lock);
    htable_new(&m->bindings, 0);
    arraylist_new(&m->usings, 0);
    JL_GC_PUSH1(&m);
    if (jl_core_module && default_names) {
        jl_module_using(m, jl_core_module);
    }
    // export own name, so "using Foo" makes "Foo" itself visible
    if (default_names) {
        jl_set_const(m, name, (jl_value_t*)m);
    }
    jl_module_export(m, name);
    JL_GC_POP();
    return m;
}

JL_DLLEXPORT jl_module_t *jl_new_module(jl_sym_t *name, jl_module_t *parent)
{
    return jl_new_module_(name, parent, 1);
}

uint32_t jl_module_next_counter(jl_module_t *m)
{
    return jl_atomic_fetch_add(&m->counter, 1);
}

JL_DLLEXPORT jl_value_t *jl_f_new_module(jl_sym_t *name, uint8_t std_imports, uint8_t default_names)
{
    // TODO: should we prohibit this during incremental compilation?
    // TODO: the parent module is a lie
    jl_module_t *m = jl_new_module_(name, jl_main_module, default_names);
    JL_GC_PUSH1(&m);
    if (std_imports)
        jl_add_standard_imports(m);
    JL_GC_POP();
    // TODO: should we somehow try to gc-root this correctly?
    return (jl_value_t*)m;
}

JL_DLLEXPORT void jl_set_module_nospecialize(jl_module_t *self, int on)
{
    self->nospecialize = (on ? -1 : 0);
}

JL_DLLEXPORT void jl_set_module_optlevel(jl_module_t *self, int lvl)
{
    self->optlevel = lvl;
}

JL_DLLEXPORT int jl_get_module_optlevel(jl_module_t *m)
{
    int lvl = m->optlevel;
    while (lvl == -1 && m->parent != m && m != jl_base_module) {
        m = m->parent;
        lvl = m->optlevel;
    }
    return lvl;
}

JL_DLLEXPORT void jl_set_module_compile(jl_module_t *self, int value)
{
    self->compile = value;
}

JL_DLLEXPORT int jl_get_module_compile(jl_module_t *m)
{
    int value = m->compile;
    while (value == -1 && m->parent != m && m != jl_base_module) {
        m = m->parent;
        value = m->compile;
    }
    return value;
}

JL_DLLEXPORT void jl_set_module_infer(jl_module_t *self, int value)
{
    self->infer = value;
    // no reason to specialize if inference is off
    if (!value)
        jl_set_module_nospecialize(self, 1);
}

JL_DLLEXPORT int jl_get_module_infer(jl_module_t *m)
{
    int value = m->infer;
    while (value == -1 && m->parent != m && m != jl_base_module) {
        m = m->parent;
        value = m->infer;
    }
    return value;
}

JL_DLLEXPORT void jl_set_module_max_methods(jl_module_t *self, int value)
{
    self->max_methods = value;
}

JL_DLLEXPORT int jl_get_module_max_methods(jl_module_t *m)
{
    int value = m->max_methods;
    while (value == -1 && m->parent != m && m != jl_base_module) {
        m = m->parent;
        value = m->max_methods;
    }
    return value;
}

JL_DLLEXPORT void jl_set_istopmod(jl_module_t *self, uint8_t isprimary)
{
    self->istopmod = 1;
    if (isprimary) {
        jl_top_module = self;
    }
}

JL_DLLEXPORT uint8_t jl_istopmod(jl_module_t *mod)
{
    return mod->istopmod;
}

static jl_globalref_t *jl_new_globalref(jl_module_t *mod, jl_sym_t *name, jl_binding_t *b)
{
    jl_task_t *ct = jl_current_task;
    jl_globalref_t *g = (jl_globalref_t*)jl_gc_alloc(ct->ptls, sizeof(jl_globalref_t), jl_globalref_type);
    g->mod = mod;
    jl_gc_wb(g, g->mod);
    g->name = name;
    g->binding = b;
    return g;
}

static jl_binding_t *new_binding(jl_module_t *mod, jl_sym_t *name)
{
    jl_task_t *ct = jl_current_task;
    assert(jl_is_module(mod) && jl_is_symbol(name));
    jl_binding_t *b = (jl_binding_t*)jl_gc_alloc(ct->ptls, sizeof(jl_binding_t), jl_binding_type);
    jl_atomic_store_relaxed(&b->value, NULL);
    b->owner = NULL;
    jl_atomic_store_relaxed(&b->ty, NULL);
    b->constp = 0;
    b->exportp = 0;
    b->imported = 0;
    b->deprecated = 0;
    b->globalref = NULL;
    JL_GC_PUSH1(&b);
    jl_atomic_store_relaxed(&b->globalref, jl_new_globalref(mod, name, b));
    JL_GC_POP();
    return b;
}

// get binding for assignment
JL_DLLEXPORT jl_binding_t *jl_get_binding_wr(jl_module_t *m JL_PROPAGATES_ROOT, jl_sym_t *var, int alloc)
{
    JL_LOCK(&m->lock);
    jl_binding_t **bp = (jl_binding_t**)ptrhash_bp(&m->bindings, var);
    jl_binding_t *b = *bp;

    if (b != HT_NOTFOUND) {
        if (b->owner != b) {
            if (b->owner == NULL) {
                b->owner = b;
            }
            else if (alloc) {
                JL_UNLOCK(&m->lock);
                jl_errorf("cannot assign a value to imported variable %s.%s",
                          jl_symbol_name(m->name), jl_symbol_name(var));
            }
        }
    }
    else if (alloc) {
        b = new_binding(m, var);
        b->owner = b;
        *bp = b;
        JL_GC_PROMISE_ROOTED(b);
        jl_gc_wb(m, b);
    }
    else {
        b = NULL;
    }

    JL_UNLOCK(&m->lock);
    return b;
}

// Hash tables don't generically root their contents, but they do for bindings.
// Express this to the analyzer.
// NOTE: Must hold m->lock while calling these.
#ifdef __clang_gcanalyzer__
jl_binding_t *_jl_get_module_binding(jl_module_t *m JL_PROPAGATES_ROOT, jl_sym_t *var) JL_NOTSAFEPOINT;
#else
static inline jl_binding_t *_jl_get_module_binding(jl_module_t *m JL_PROPAGATES_ROOT, jl_sym_t *var) JL_NOTSAFEPOINT
{
    return (jl_binding_t*)ptrhash_get(&m->bindings, var);
}
#endif

// return module of binding
JL_DLLEXPORT jl_module_t *jl_get_module_of_binding(jl_module_t *m, jl_sym_t *var)
{
    jl_binding_t *b = jl_get_binding(m, var);
    if (b == NULL)
        return NULL;
    return b->globalref->mod; // TODO: deprecate this?
}

// get binding for adding a method
// like jl_get_binding_wr, but has different error paths
JL_DLLEXPORT jl_binding_t *jl_get_binding_for_method_def(jl_module_t *m, jl_sym_t *var)
{
    JL_LOCK(&m->lock);
    jl_binding_t **bp = (jl_binding_t**)ptrhash_bp(&m->bindings, var);
    jl_binding_t *b = *bp;
    JL_GC_PROMISE_ROOTED(b);

    if (b != HT_NOTFOUND) {
        JL_UNLOCK(&m->lock);
        jl_binding_t *b2 = b->owner;
        if (b2 != b) {
            // TODO: make this cmpswap atomic
            if (b2 == NULL) {
                b->owner = b;
            }
            else {
                assert(jl_atomic_load_relaxed(&b2->value) != NULL);
                // TODO: we might want to require explicitly importing types to add constructors
                if (!b->imported && (!b2->constp || !jl_is_type(jl_atomic_load_relaxed(&b2->value)))) {
                    jl_errorf("error in method definition: function %s.%s must be explicitly imported to be extended",
                              jl_symbol_name(m->name), jl_symbol_name(var));
                }
                return b2;
            }
        }
    }
    else {
        b = new_binding(m, var);
        b->owner = b;
        *bp = b;
        JL_GC_PROMISE_ROOTED(b);
        jl_gc_wb(m, b);
        JL_UNLOCK(&m->lock); // may gc
    }

    return b;
}

static void module_import_(jl_module_t *to, jl_module_t *from, jl_binding_t *b, jl_sym_t *asname, jl_sym_t *s, int explici);

typedef struct _modstack_t {
    jl_module_t *m;
    jl_sym_t *var;
    struct _modstack_t *prev;
} modstack_t;

static jl_binding_t *jl_resolve_owner(jl_binding_t *b/*optional*/, jl_module_t *m JL_PROPAGATES_ROOT, jl_sym_t *var, modstack_t *st);

static inline jl_module_t *module_usings_getidx(jl_module_t *m JL_PROPAGATES_ROOT, size_t i) JL_NOTSAFEPOINT;

#ifndef __clang_gcanalyzer__
// The analyzer doesn't like looking through the arraylist, so just model the
// access for it using this function
static inline jl_module_t *module_usings_getidx(jl_module_t *m JL_PROPAGATES_ROOT, size_t i) JL_NOTSAFEPOINT {
    return (jl_module_t*)m->usings.items[i];
}
#endif

static int eq_bindings(jl_binding_t *a, jl_binding_t *b)
{
    if (a == b)
        return 1;
    if (a->owner == b->owner)
        return 1;
    if (a->constp && b->constp && jl_atomic_load_relaxed(&a->value) && jl_atomic_load_relaxed(&b->value) == jl_atomic_load_relaxed(&a->value))
        return 1;
    return 0;
}

// find a binding from a module's `usings` list
// called while holding m->lock
static jl_binding_t *using_resolve_binding(jl_module_t *m JL_PROPAGATES_ROOT, jl_sym_t *var, jl_module_t **from, modstack_t *st, int warn)
{
    jl_binding_t *b = NULL;
    jl_module_t *owner = NULL;
    for (int i = (int)m->usings.len - 1; i >= 0; --i) {
        jl_module_t *imp = module_usings_getidx(m, i);
        // TODO: make sure this can't deadlock
        jl_binding_t *tempb = jl_get_module_binding(imp, var);
        if (tempb != NULL && tempb->exportp) {
            tempb = jl_resolve_owner(NULL, imp, var, st); // find the owner for tempb
            if (tempb == NULL)
                // couldn't resolve; try next using (see issue #6105)
                continue;
            assert(tempb->owner == tempb);
            if (b != NULL && !tempb->deprecated && !b->deprecated && !eq_bindings(tempb, b)) {
                if (warn) {
                    // mark this binding resolved (by creating it or setting the owner), to avoid repeating the warning
                    (void)jl_get_binding_wr(m, var, 1);
                    JL_UNLOCK(&m->lock);
                    jl_printf(JL_STDERR,
                              "WARNING: both %s and %s export \"%s\"; uses of it in module %s must be qualified\n",
                              jl_symbol_name(owner->name),
                              jl_symbol_name(imp->name), jl_symbol_name(var),
                              jl_symbol_name(m->name));
                    JL_LOCK(&m->lock);
                }
                return NULL;
            }
            if (owner == NULL || !tempb->deprecated) {
                owner = imp;
                b = tempb;
            }
        }
    }
    *from = owner;
    return b;
}

// get binding for reading. might return NULL for unbound.
static jl_binding_t *jl_resolve_owner(jl_binding_t *b/*optional*/, jl_module_t *m, jl_sym_t *var, modstack_t *st)
{
    if (b == NULL) {
        b = jl_get_module_binding(m, var);
        if (b != NULL)
            b = b->owner;
    }
    else {
        b = b->owner;
    }
    if (b == NULL) {
        modstack_t top = { m, var, st };
        modstack_t *tmp = st;
        for (; tmp != NULL; tmp = tmp->prev) {
            if (tmp->m == m && tmp->var == var) {
                // import cycle without finding actual location
                return NULL;
            }
        }
        jl_module_t *from = NULL; // for error message printing
        JL_LOCK(&m->lock);
        b = using_resolve_binding(m, var, &from, &top, 1);
        JL_UNLOCK(&m->lock);
        if (b != NULL) {
            // do a full import to prevent the result of this lookup
            // from changing, for example if this var is assigned to
            // later.
            // TODO: make this more thread-safe
            assert(b->owner == b && from);
            module_import_(m, from, b, var, var, 0);
            return b;
        }
        return NULL;
    }
    assert(b->owner == b);
    return b;
}

JL_DLLEXPORT jl_binding_t *jl_get_binding_if_bound(jl_module_t *m, jl_sym_t *var)
{
    jl_binding_t *b = jl_get_module_binding(m, var);
    return b == NULL ? NULL : b->owner;
}


// get the current likely owner of binding when accessing m.var, without resolving the binding (it may change later)
JL_DLLEXPORT jl_binding_t *jl_binding_owner(jl_module_t *m, jl_sym_t *var)
{
    JL_LOCK(&m->lock);
    jl_binding_t *b = (jl_binding_t*)ptrhash_get(&m->bindings, var);
    jl_module_t *from = m;
    if (b == HT_NOTFOUND || b->owner == NULL) {
        b = using_resolve_binding(m, var, &from, NULL, 0);
        if (b == NULL)
            return NULL;
    }
    else {
        b = b->owner;
    }
    JL_UNLOCK(&m->lock);
    return b;
}

// get type of binding m.var, without resolving the binding
JL_DLLEXPORT jl_value_t *jl_get_binding_type(jl_module_t *m, jl_sym_t *var)
{
    jl_binding_t *b = jl_get_module_binding(m, var);
    if (b == NULL || b->owner == NULL)
        return jl_nothing;
    jl_value_t *ty = jl_atomic_load_relaxed(&b->owner->ty);
    return ty ? ty : jl_nothing;
}

JL_DLLEXPORT jl_binding_t *jl_get_binding_wr_or_error(jl_module_t *m, jl_sym_t *var)
{
    return jl_get_binding_wr(m, var, 1);
}

JL_DLLEXPORT jl_binding_t *jl_get_binding(jl_module_t *m, jl_sym_t *var)
{
    return jl_resolve_owner(NULL, m, var, NULL);
}

JL_DLLEXPORT jl_binding_t *jl_get_binding_or_error(jl_module_t *m, jl_sym_t *var)
{
    jl_binding_t *b = jl_get_binding(m, var);
    if (b == NULL)
        jl_undefined_var_error(var);
    // XXX: this only considers if the original is deprecated, not the binding in m
    if (b->deprecated)
        jl_binding_deprecation_warning(m, var, b);
    return b;
}

JL_DLLEXPORT jl_value_t *jl_module_globalref(jl_module_t *m, jl_sym_t *var)
{
    JL_LOCK(&m->lock);
    jl_binding_t *b = _jl_get_module_binding(m, var);
    if (b == HT_NOTFOUND) {
        b = new_binding(m, var);
        ptrhash_put(&m->bindings, (void*)var, (void*)b);
        jl_gc_wb(m, b);
        JL_GC_PROMISE_ROOTED(b);
    }
    JL_UNLOCK(&m->lock); // may GC
    jl_globalref_t *globalref = jl_atomic_load_relaxed(&b->globalref);
    assert(globalref != NULL);
    return (jl_value_t*)globalref;
}

// does module m explicitly import s?
JL_DLLEXPORT int jl_is_imported(jl_module_t *m, jl_sym_t *s)
{
    JL_LOCK(&m->lock);
    jl_binding_t *b = (jl_binding_t*)ptrhash_get(&m->bindings, s);
    JL_UNLOCK(&m->lock);
    return (b != HT_NOTFOUND && b->imported);
}

extern const char *jl_filename;
extern int jl_lineno;

static char const dep_message_prefix[] = "_dep_message_";

static void jl_binding_dep_message(jl_module_t *m, jl_sym_t *name, jl_binding_t *b)
{
    size_t prefix_len = strlen(dep_message_prefix);
    size_t name_len = strlen(jl_symbol_name(name));
    char *dep_binding_name = (char*)alloca(prefix_len+name_len+1);
    memcpy(dep_binding_name, dep_message_prefix, prefix_len);
    memcpy(dep_binding_name + prefix_len, jl_symbol_name(name), name_len);
    dep_binding_name[prefix_len+name_len] = '\0';
    jl_binding_t *dep_message_binding = jl_get_binding(m, jl_symbol(dep_binding_name));
    jl_value_t *dep_message = NULL;
    if (dep_message_binding != NULL)
        dep_message = jl_atomic_load_relaxed(&dep_message_binding->value);
    JL_GC_PUSH1(&dep_message);
    if (dep_message != NULL) {
        if (jl_is_string(dep_message)) {
            jl_uv_puts(JL_STDERR, jl_string_data(dep_message), jl_string_len(dep_message));
        }
        else {
            jl_static_show(JL_STDERR, dep_message);
        }
    }
    else {
        jl_value_t *v = jl_atomic_load_relaxed(&b->value);
        dep_message = v; // use as gc-root
        if (v) {
            if (jl_is_type(v) || jl_is_module(v)) {
                jl_printf(JL_STDERR, ", use ");
                jl_static_show(JL_STDERR, v);
                jl_printf(JL_STDERR, " instead.");
            }
            else {
                jl_methtable_t *mt = jl_gf_mtable(v);
                if (mt != NULL) {
                    jl_printf(JL_STDERR, ", use ");
                    if (mt->module != jl_core_module) {
                        jl_static_show(JL_STDERR, (jl_value_t*)mt->module);
                        jl_printf(JL_STDERR, ".");
                    }
                    jl_printf(JL_STDERR, "%s", jl_symbol_name(mt->name));
                    jl_printf(JL_STDERR, " instead.");
                }
            }
        }
    }
    jl_printf(JL_STDERR, "\n");
    JL_GC_POP();
}

// NOTE: we use explici since explicit is a C++ keyword
static void module_import_(jl_module_t *to, jl_module_t *from, jl_binding_t *b, jl_sym_t *asname, jl_sym_t *s, int explici)
{
    if (b == NULL) {
        jl_printf(JL_STDERR,
                  "WARNING: could not import %s.%s into %s\n",
                  jl_symbol_name(from->name), jl_symbol_name(s),
                  jl_symbol_name(to->name));
    }
    else {
        assert(b->owner == b);
        if (b->deprecated) {
            if (jl_atomic_load_relaxed(&b->value) == jl_nothing) {
                // silently skip importing deprecated values assigned to nothing (to allow later mutation)
                return;
            }
            else if (to != jl_main_module && to != jl_base_module &&
                     jl_options.depwarn != JL_OPTIONS_DEPWARN_OFF) {
                /* with #22763, external packages wanting to replace
                   deprecated Base bindings should simply export the new
                   binding */
                jl_printf(JL_STDERR,
                          "WARNING: importing deprecated binding %s.%s into %s%s%s.\n",
                          jl_symbol_name(from->name), jl_symbol_name(s),
                          jl_symbol_name(to->name),
                          asname == s ? "" : " as ",
                          asname == s ? "" : jl_symbol_name(asname));
                jl_binding_dep_message(from, s, b);
            }
        }

        JL_LOCK(&to->lock);
        jl_binding_t **bp = (jl_binding_t**)ptrhash_bp(&to->bindings, asname);
        jl_binding_t *bto = *bp;
        if (bto != HT_NOTFOUND) {
            JL_GC_PROMISE_ROOTED(bto);
            if (bto == b) {
                // importing a binding on top of itself. harmless.
            }
            else if (eq_bindings(bto, b)) {
                // already imported
                bto->imported = (explici != 0);
            }
            else if (bto->owner != b && bto->owner != NULL) {
                // already imported from somewhere else
                JL_UNLOCK(&to->lock);
                jl_printf(JL_STDERR,
                          "WARNING: ignoring conflicting import of %s.%s into %s\n",
                          jl_symbol_name(from->name), jl_symbol_name(s),
                          jl_symbol_name(to->name));
                return;
            }
            else if (bto->constp || jl_atomic_load_relaxed(&bto->value)) {
                // conflict with name owned by destination module
                assert(bto->owner == bto);
                JL_UNLOCK(&to->lock);
                jl_printf(JL_STDERR,
                          "WARNING: import of %s.%s into %s conflicts with an existing identifier; ignored.\n",
                          jl_symbol_name(from->name), jl_symbol_name(s),
                          jl_symbol_name(to->name));
                return;
            }
            else {
                bto->owner = b->owner;
                bto->imported = (explici != 0);
            }
        }
        else {
            jl_binding_t *nb = new_binding(to, asname);
            nb->owner = b;
            nb->imported = (explici != 0);
            nb->deprecated = b->deprecated; // we already warned about this above, but we might want to warn at the use sites too
            *bp = nb;
            jl_gc_wb(to, nb);
        }
        JL_UNLOCK(&to->lock);
    }
}

JL_DLLEXPORT void jl_module_import(jl_module_t *to, jl_module_t *from, jl_sym_t *s)
{
    jl_binding_t *b = jl_get_binding(from, s);
    module_import_(to, from, b, s, s, 1);
}

JL_DLLEXPORT void jl_module_import_as(jl_module_t *to, jl_module_t *from, jl_sym_t *s, jl_sym_t *asname)
{
    jl_binding_t *b = jl_get_binding(from, s);
    module_import_(to, from, b, asname, s, 1);
}

JL_DLLEXPORT void jl_module_use(jl_module_t *to, jl_module_t *from, jl_sym_t *s)
{
    jl_binding_t *b = jl_get_binding(from, s);
    module_import_(to, from, b, s, s, 0);
}

JL_DLLEXPORT void jl_module_use_as(jl_module_t *to, jl_module_t *from, jl_sym_t *s, jl_sym_t *asname)
{
    jl_binding_t *b = jl_get_binding(from, s);
    module_import_(to, from, b, asname, s, 0);
}

JL_DLLEXPORT void jl_module_using(jl_module_t *to, jl_module_t *from)
{
    if (to == from)
        return;
    JL_LOCK(&to->lock);
    for (size_t i = 0; i < to->usings.len; i++) {
        if (from == to->usings.items[i]) {
            JL_UNLOCK(&to->lock);
            return;
        }
    }
    // TODO: make sure this can't deadlock
    JL_LOCK(&from->lock);
    // print a warning if something visible via this "using" conflicts with
    // an existing identifier. note that an identifier added later may still
    // silently override a "using" name. see issue #2054.
    void **table = from->bindings.table;
    for (size_t i = 1; i < from->bindings.size; i += 2) {
        if (table[i] != HT_NOTFOUND) {
            jl_binding_t *b = (jl_binding_t*)table[i];
            if (b->exportp && (b->owner == b || b->imported)) {
                jl_sym_t *var = (jl_sym_t*)table[i-1];
                jl_binding_t **tobp = (jl_binding_t**)ptrhash_bp(&to->bindings, var);
                if (*tobp != HT_NOTFOUND && (*tobp)->owner != NULL &&
                    // don't warn for conflicts with the module name itself.
                    // see issue #4715
                    var != to->name &&
                    !eq_bindings(jl_get_binding(to, var), b)) {
                    // TODO: not ideal to print this while holding module locks
                    jl_printf(JL_STDERR,
                              "WARNING: using %s.%s in module %s conflicts with an existing identifier.\n",
                              jl_symbol_name(from->name), jl_symbol_name(var),
                              jl_symbol_name(to->name));
                }
            }
        }
    }
    JL_UNLOCK(&from->lock);

    arraylist_push(&to->usings, from);
    jl_gc_wb(to, from);
    JL_UNLOCK(&to->lock);
}

JL_DLLEXPORT void jl_module_export(jl_module_t *from, jl_sym_t *s)
{
    JL_LOCK(&from->lock);
    jl_binding_t **bp = (jl_binding_t**)ptrhash_bp(&from->bindings, s);
    if (*bp == HT_NOTFOUND) {
        jl_binding_t *b = new_binding(from, s);
        // don't yet know who the owner will be
        *bp = b;
        jl_gc_wb(from, b);
    }
    assert(*bp != HT_NOTFOUND);
    (*bp)->exportp = 1;
    JL_UNLOCK(&from->lock);
}

JL_DLLEXPORT int jl_boundp(jl_module_t *m, jl_sym_t *var)
{
    jl_binding_t *b = jl_get_binding(m, var);
    return b && (jl_atomic_load_relaxed(&b->value) != NULL);
}

JL_DLLEXPORT int jl_defines_or_exports_p(jl_module_t *m, jl_sym_t *var)
{
    JL_LOCK(&m->lock);
    jl_binding_t *b = (jl_binding_t*)ptrhash_get(&m->bindings, var);
    JL_UNLOCK(&m->lock);
    return b != HT_NOTFOUND && (b->exportp || b->owner == b);
}

JL_DLLEXPORT int jl_module_exports_p(jl_module_t *m, jl_sym_t *var)
{
    jl_binding_t *b = jl_get_module_binding(m, var);
    return b && b->exportp;
}

JL_DLLEXPORT int jl_binding_resolved_p(jl_module_t *m, jl_sym_t *var)
{
    jl_binding_t *b = jl_get_module_binding(m, var);
    return b && b->owner != NULL;
}

JL_DLLEXPORT jl_binding_t *jl_get_module_binding(jl_module_t *m JL_PROPAGATES_ROOT, jl_sym_t *var)
{
    JL_LOCK(&m->lock);
    jl_binding_t *b = _jl_get_module_binding(m, var);
    JL_UNLOCK(&m->lock);
    return b == HT_NOTFOUND ? NULL : b;
}


JL_DLLEXPORT jl_value_t *jl_get_globalref_value(jl_globalref_t *gr)
{
    jl_binding_t *b = gr->binding;
    b = jl_resolve_owner(b, gr->mod, gr->name, NULL);
    // ignores b->deprecated
    return b == NULL ? NULL : jl_atomic_load_relaxed(&b->value);
}

JL_DLLEXPORT jl_value_t *jl_get_global(jl_module_t *m, jl_sym_t *var)
{
    jl_binding_t *b = jl_get_binding(m, var);
    if (b == NULL) return NULL;
    // XXX: this only considers if the original is deprecated, not the binding in m
    if (b->deprecated) jl_binding_deprecation_warning(m, var, b);
    return jl_atomic_load_relaxed(&b->value);
}

JL_DLLEXPORT void jl_set_global(jl_module_t *m JL_ROOTING_ARGUMENT, jl_sym_t *var, jl_value_t *val JL_ROOTED_ARGUMENT)
{
    jl_binding_t *bp = jl_get_binding_wr(m, var, 1);
    jl_checked_assignment(bp, m, var, val);
}

JL_DLLEXPORT void jl_set_const(jl_module_t *m JL_ROOTING_ARGUMENT, jl_sym_t *var, jl_value_t *val JL_ROOTED_ARGUMENT)
{
    // this function is mostly only used during initialization, so the data races here are not too important to us
    jl_binding_t *bp = jl_get_binding_wr(m, var, 1);
    if (jl_atomic_load_relaxed(&bp->value) == NULL) {
        jl_value_t *old_ty = NULL;
        jl_atomic_cmpswap_relaxed(&bp->ty, &old_ty, (jl_value_t*)jl_any_type);
        uint8_t constp = 0;
        // if (jl_atomic_cmpswap(&bp->constp, &constp, 1)) {
        if (constp = bp->constp, bp->constp = 1, constp == 0) {
            jl_value_t *old = NULL;
            if (jl_atomic_cmpswap(&bp->value, &old, val)) {
                jl_gc_wb_binding(bp, val);
                return;
            }
        }
    }
    jl_errorf("invalid redefinition of constant %s", jl_symbol_name(var));
}

JL_DLLEXPORT int jl_globalref_is_const(jl_globalref_t *gr)
{
    jl_binding_t *b = gr->binding;
    b = jl_resolve_owner(b, gr->mod, gr->name, NULL);
    return b && b->constp;
}

JL_DLLEXPORT int jl_globalref_boundp(jl_globalref_t *gr)
{
    jl_binding_t *b = gr->binding;
    b = jl_resolve_owner(b, gr->mod, gr->name, NULL);
    return b && (jl_atomic_load_relaxed(&b->value) != NULL);
}

JL_DLLEXPORT int jl_is_const(jl_module_t *m, jl_sym_t *var)
{
    jl_binding_t *b = jl_get_binding(m, var);
    return b && b->constp;
}

// set the deprecated flag for a binding:
//   0=not deprecated, 1=renamed, 2=moved to another package
JL_DLLEXPORT void jl_deprecate_binding(jl_module_t *m, jl_sym_t *var, int flag)
{
    // XXX: this deprecates the original value, which might be imported from elsewhere
    jl_binding_t *b = jl_get_binding(m, var);
    if (b) b->deprecated = flag;
}

JL_DLLEXPORT int jl_is_binding_deprecated(jl_module_t *m, jl_sym_t *var)
{
    if (jl_binding_resolved_p(m, var)) {
        // XXX: this only considers if the original is deprecated, not this precise binding
        jl_binding_t *b = jl_get_binding(m, var);
        return b && b->deprecated;
    }
    return 0;
}


void jl_binding_deprecation_warning(jl_module_t *m, jl_sym_t *s, jl_binding_t *b)
{
    // Only print a warning for deprecated == 1 (renamed).
    // For deprecated == 2 (moved to a package) the binding is to a function
    // that throws an error, so we don't want to print a warning too.
    if (b->deprecated == 1 && jl_options.depwarn) {
        if (jl_options.depwarn != JL_OPTIONS_DEPWARN_ERROR)
            jl_printf(JL_STDERR, "WARNING: ");
        assert(b->owner == b);
        jl_printf(JL_STDERR, "%s.%s is deprecated",
                  jl_symbol_name(m->name), jl_symbol_name(s));
        jl_binding_dep_message(m, s, b);

        if (jl_options.depwarn != JL_OPTIONS_DEPWARN_ERROR) {
            if (jl_lineno == 0) {
                jl_printf(JL_STDERR, " in module %s\n", jl_symbol_name(m->name));
            }
            else {
                jl_printf(JL_STDERR, "  likely near %s:%d\n", jl_filename, jl_lineno);
            }
        }

        if (jl_options.depwarn == JL_OPTIONS_DEPWARN_ERROR) {
            jl_errorf("use of deprecated variable: %s.%s",
                      jl_symbol_name(m->name),
                      jl_symbol_name(s));
        }
    }
}

JL_DLLEXPORT void jl_checked_assignment(jl_binding_t *b, jl_module_t *mod, jl_sym_t *var, jl_value_t *rhs)
{
    // TODO: pass name as an argument
    jl_value_t *old_ty = NULL;
    if (!jl_atomic_cmpswap_relaxed(&b->ty, &old_ty, (jl_value_t*)jl_any_type)) {
        if (old_ty != (jl_value_t*)jl_any_type && jl_typeof(rhs) != old_ty) {
            JL_GC_PUSH1(&rhs);
            if (!jl_isa(rhs, old_ty))
                jl_errorf("cannot assign an incompatible value to the global %s.%s.",
                          jl_symbol_name(mod->name), jl_symbol_name(var));
            JL_GC_POP();
        }
    }
    if (b->constp) {
        jl_value_t *old = NULL;
        if (jl_atomic_cmpswap(&b->value, &old, rhs)) {
            jl_gc_wb_binding(b, rhs);
            return;
        }
        if (jl_egal(rhs, old))
            return;
        if (jl_typeof(rhs) != jl_typeof(old) || jl_is_type(rhs) || jl_is_module(rhs)) {
#ifndef __clang_gcanalyzer__
            jl_errorf("invalid redefinition of constant %s.%s",
                      jl_symbol_name(mod->name), jl_symbol_name(var));

#endif
        }
        jl_safe_printf("WARNING: redefinition of constant %s.%s. This may fail, cause incorrect answers, or produce other errors.\n",
                       jl_symbol_name(mod->name), jl_symbol_name(var));
    }
    jl_atomic_store_release(&b->value, rhs);
    jl_gc_wb_binding(b, rhs);
}

JL_DLLEXPORT void jl_declare_constant(jl_binding_t *b, jl_module_t *mod, jl_sym_t *var)
{
    if (b->owner != b || (jl_atomic_load_relaxed(&b->value) != NULL && !b->constp)) {
        jl_errorf("cannot declare %s constant; it already has a value",
                  jl_symbol_name(mod->name), jl_symbol_name(var));
    }
    b->constp = 1;
}

JL_DLLEXPORT jl_value_t *jl_module_usings(jl_module_t *m)
{
    jl_array_t *a = jl_alloc_array_1d(jl_array_any_type, 0);
    JL_GC_PUSH1(&a);
    JL_LOCK(&m->lock);
    for(int i=(int)m->usings.len-1; i >= 0; --i) {
        jl_array_grow_end(a, 1);
        jl_module_t *imp = (jl_module_t*)m->usings.items[i];
        jl_array_ptr_set(a,jl_array_dim0(a)-1, (jl_value_t*)imp);
    }
    JL_UNLOCK(&m->lock);
    JL_GC_POP();
    return (jl_value_t*)a;
}

JL_DLLEXPORT jl_value_t *jl_module_names(jl_module_t *m, int all, int imported)
{
    jl_array_t *a = jl_alloc_array_1d(jl_array_symbol_type, 0);
    JL_GC_PUSH1(&a);
    size_t i;
    JL_LOCK(&m->lock);
    void **table = m->bindings.table;
    for (i = 0; i < m->bindings.size; i+=2) {
        if (table[i+1] != HT_NOTFOUND) {
            jl_sym_t *asname = (jl_sym_t*)table[i];
            jl_binding_t *b = (jl_binding_t*)table[i+1];
            int hidden = jl_symbol_name(asname)[0]=='#';
            if ((b->exportp ||
                 (imported && b->imported) ||
                 (b->owner == b && !b->imported && (all || m == jl_main_module))) &&
                (all || (!b->deprecated && !hidden))) {
                jl_array_grow_end(a, 1);
                // n.b. change to jl_arrayset if array storage allocation for Array{Symbols,1} changes:
                jl_array_ptr_set(a, jl_array_dim0(a)-1, (jl_value_t*)asname);
            }
        }
    }
    JL_UNLOCK(&m->lock);
    JL_GC_POP();
    return (jl_value_t*)a;
}

JL_DLLEXPORT jl_sym_t *jl_module_name(jl_module_t *m) { return m->name; }
JL_DLLEXPORT jl_module_t *jl_module_parent(jl_module_t *m) { return m->parent; }
JL_DLLEXPORT jl_uuid_t jl_module_build_id(jl_module_t *m) { return m->build_id; }
JL_DLLEXPORT jl_uuid_t jl_module_uuid(jl_module_t* m) { return m->uuid; }

// TODO: make this part of the module constructor and read-only?
JL_DLLEXPORT void jl_set_module_uuid(jl_module_t *m, jl_uuid_t uuid) { m->uuid = uuid; }

int jl_is_submodule(jl_module_t *child, jl_module_t *parent) JL_NOTSAFEPOINT
{
    while (1) {
        if (parent == child)
            return 1;
        if (child == NULL || child == child->parent)
            return 0;
        child = child->parent;
    }
}

// Remove implicitly imported identifiers, effectively resetting all the binding
// resolution decisions for a module. This is dangerous, and should only be
// done for modules that are essentially empty anyway. The only use case for this
// is to leave `Main` as empty as possible in the default system image.
JL_DLLEXPORT void jl_clear_implicit_imports(jl_module_t *m)
{
    size_t i;
    JL_LOCK(&m->lock);
    void **table = m->bindings.table;
    for (i = 1; i < m->bindings.size; i+=2) {
        if (table[i] != HT_NOTFOUND) {
            jl_binding_t *b = (jl_binding_t*)table[i];
            if (b->owner != b && !b->imported)
                table[i] = HT_NOTFOUND;
        }
    }
    JL_UNLOCK(&m->lock);
}

JL_DLLEXPORT void jl_init_restored_modules(jl_array_t *init_order)
{
    int i, l = jl_array_len(init_order);
    for (i = 0; i < l; i++) {
        jl_value_t *mod = jl_array_ptr_ref(init_order, i);
        if (!jl_generating_output() || jl_options.incremental) {
            jl_module_run_initializer((jl_module_t*)mod);
        }
        else {
            if (jl_module_init_order == NULL)
                jl_module_init_order = jl_alloc_vec_any(0);
            jl_array_ptr_1d_push(jl_module_init_order, mod);
        }
    }
}

#ifdef __cplusplus
}
#endif

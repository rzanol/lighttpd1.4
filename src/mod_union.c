#include "first.h"

#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "plugin.h"

#include "array.h"
#include "buffer.h"
#include "log.h"
#include "request.h"
#include "stat_cache.h"

typedef struct {
    const array *prefixes; // Map of prefix => array of replacements
} plugin_config;

typedef struct {
    PLUGIN_DATA;
    plugin_config defaults;
    plugin_config conf;
} plugin_data;

static data_unset *array_match_path_prefix(const array * const a, const buffer * const b) {
    const uint32_t blen = buffer_clen(b);

    for (uint32_t i = 0; i < a->used; ++i) {
        const buffer * const key = &a->data[i]->key;
        const uint32_t klen = buffer_clen(key);

        if (klen <= blen && memcmp(b->ptr, key->ptr, klen) == 0) {
            // The prefix matches, but check that the path component is not longer (e.g. /test vs /testing)
            if (klen < blen && !buffer_has_slash_suffix(key) && b->ptr[klen] != '/') continue;
            return a->data[i];
        }
    }

    return NULL;
}

static int replace_path(buffer *path, const array * const prefixes) {
    if (buffer_is_blank(path)) return 0;

    const data_array * const match = (const data_array *)array_match_path_prefix(prefixes, path);
    if (!match) return 0;

    const uint32_t key_len = buffer_clen(&match->key);
    const char * const suffix = path->ptr + key_len;
    const uint32_t suffix_len = buffer_clen(path) - key_len;

    int found = 0;
    for (uint32_t i = 0; !found && i < match->value.used; ++i) {
        const buffer * const subst_prefix = &((const data_string * )match->value.data[i])->value;

        buffer *new_path = buffer_init();
        buffer_copy_path_len2(new_path, subst_prefix->ptr, buffer_clen(subst_prefix), suffix, suffix_len);

        // Keep the same trailing slash as the original path
        if (buffer_has_slash_suffix(new_path) && !buffer_has_slash_suffix(path)) {
            new_path->used--;
            new_path->ptr[new_path->used - 1] = '\0';
        } else if (!buffer_has_slash_suffix(new_path) && buffer_has_slash_suffix(path)) {
            buffer_append_string(new_path, "/");
        }

        const stat_cache_entry * const sce = stat_cache_get_entry(new_path);
        if (NULL != sce) {
            buffer_copy_string_len(path, new_path->ptr, buffer_clen(new_path));
            found = 1;
        }

        buffer_free(new_path);
    }

    return 0;
}

INIT_FUNC(mod_union_init) {
    return ck_calloc(1, sizeof(plugin_data));
}

static void mod_union_merge_config_cpv(plugin_config * const pconf, const config_plugin_value_t * const cpv) {
    switch (cpv->k_id) { /* index into static config_plugin_keys_t cpk[] */
      case 0: /* union.prefixes */
       pconf->prefixes = cpv->v.a;
        break;
      default:/* should not happen */
        return;
    }
}

static void mod_union_merge_config(plugin_config * const pconf, const config_plugin_value_t *cpv) {
    do {
        mod_union_merge_config_cpv(pconf, cpv);
    } while ((++cpv)->k_id != -1);
}

static void mod_union_patch_config(request_st * const r, plugin_data * const p) {
    p->conf = p->defaults;
    for (int i = 1, used = p->nconfig; i < used; ++i) {
        if (config_check_cond(r, (uint32_t)p->cvlist[i].k_id))
            mod_union_merge_config(&p->conf, p->cvlist+p->cvlist[i].v.u2[0]);
    }
}

SETDEFAULTS_FUNC(mod_union_set_defaults) {
    static const config_plugin_keys_t cpk[] = {
      { CONST_STR_LEN("union.prefixes"),
        T_CONFIG_ARRAY_KVARRAY,
        T_CONFIG_SCOPE_CONNECTION }
     ,{ NULL, 0,
        T_CONFIG_UNSET,
        T_CONFIG_SCOPE_UNSET }
    };

    plugin_data * const p = p_d;
    if (!config_plugin_values_init(srv, p, cpk, "mod_union"))
        return HANDLER_ERROR;

    /* initialize p->defaults from global config context */
    if (p->nconfig > 0 && p->cvlist->v.u2[1]) {
        const config_plugin_value_t *cpv = p->cvlist + p->cvlist->v.u2[0];
        if (-1 != cpv->k_id)
            mod_union_merge_config(&p->defaults, cpv);
    }

    return HANDLER_GO_ON;
}

URIHANDLER_FUNC(mod_union_physical_handler) {
    plugin_data * const p = p_d;

    if (NULL != r->handler_module) return HANDLER_GO_ON;

    mod_union_patch_config(r, p);

    if (0 != replace_path(&r->physical.path, p->conf.prefixes)) return HANDLER_ERROR;
    if (0 != replace_path(&r->physical.basedir, p->conf.prefixes)) return HANDLER_ERROR;

    return HANDLER_GO_ON;
}


__attribute_cold__
__declspec_dllexport__
int mod_union_plugin_init(plugin *p);
int mod_union_plugin_init(plugin *p) {
    p->version     = LIGHTTPD_VERSION_ID;
    p->name        = "union";

    p->init             = mod_union_init;
    p->set_defaults     = mod_union_set_defaults;
    p->handle_physical  = mod_union_physical_handler;

    return 0;
}

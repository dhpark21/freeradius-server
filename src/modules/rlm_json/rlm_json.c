/*
 *   This program is is free software; you can redistribute it and/or modify
 *   it under the terms of the GNU General Public License as published by
 *   the Free Software Foundation; either version 2 of the License, or (at
 *   your option) any later version.
 *
 *   This program is distributed in the hope that it will be useful,
 *   but WITHOUT ANY WARRANTY; without even the implied warranty of
 *   MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *   GNU General Public License for more details.
 *
 *   You should have received a copy of the GNU General Public License
 *   along with this program; if not, write to the Free Software
 *   Foundation, Inc., 51 Franklin St, Fifth Floor, Boston, MA 02110-1301, USA
 */

/**
 * $Id$
 * @file rlm_json.c
 * @brief Parses JSON responses
 *
 * @author Arran Cudbard-Bell
 * @author Matthew Newton
 *
 * @copyright 2015 Arran Cudbard-Bell (a.cudbardb@freeradius.org)
 * @copyright 2015,2020 Network RADIUS SAS (legal@networkradius.com)
 * @copyright 2015 The FreeRADIUS Server Project
 */
RCSID("$Id$")

#include <freeradius-devel/server/base.h>
#include <freeradius-devel/server/module_rlm.h>
#include <freeradius-devel/server/map_proc.h>
#include <freeradius-devel/util/base16.h>
#include <freeradius-devel/util/debug.h>
#include <freeradius-devel/util/sbuff.h>
#include <freeradius-devel/util/types.h>
#include <freeradius-devel/util/value.h>
#include <freeradius-devel/unlang/xlat_func.h>
#include <freeradius-devel/json/base.h>

#ifndef HAVE_JSON
#  error "rlm_json should not be built unless json-c is available"
#endif

static fr_sbuff_parse_rules_t const json_arg_parse_rules = {
	.terminals = &FR_SBUFF_TERMS(
		L("\t"),
		L(" "),
		L("!")
	)
};

/** rlm_json module instance
 *
 */
typedef struct {
	fr_json_format_t	*format;
} rlm_json_t;


static conf_parser_t const module_config[] = {
	{ FR_CONF_OFFSET_SUBSECTION("encode", 0, rlm_json_t, format, fr_json_format_config),
	  .subcs_size = sizeof(fr_json_format_t), .subcs_type = "fr_json_format_t" },

	CONF_PARSER_TERMINATOR
};

/** Forms a linked list of jpath head node pointers (a list of jpaths)
 */
typedef struct rlm_json_jpath_cache rlm_json_jpath_cache_t;
struct rlm_json_jpath_cache {
	fr_jpath_node_t		*jpath;		//!< First node in jpath expression.
	rlm_json_jpath_cache_t	*next;		//!< Next jpath cache entry.
};

typedef struct {
	fr_jpath_node_t const	*jpath;
	json_object		*root;
} rlm_json_jpath_to_eval_t;

static xlat_arg_parser_t const json_escape_xlat_arg[] = {
	{ .type = FR_TYPE_VOID },
	XLAT_ARG_PARSER_TERMINATOR
};

static xlat_action_t json_escape(TALLOC_CTX *ctx, fr_dcursor_t *out,
				 UNUSED xlat_ctx_t const *xctx,
				 request_t *request, fr_value_box_list_t *in, bool quote)
{
	fr_value_box_t	*vb_out;
	fr_value_box_t	*in_head = fr_value_box_list_head(in);
	fr_sbuff_t	*agg;

	FR_SBUFF_TALLOC_THREAD_LOCAL(&agg, 1024, SIZE_MAX);

	if (fr_value_box_list_num_elements(&in_head->vb_group) == 0) {
		MEM(vb_out = fr_value_box_alloc_null(ctx));
		fr_value_box_strdup(vb_out, vb_out, NULL, "null", false);
		fr_dcursor_append(out, vb_out);

		return XLAT_ACTION_DONE;
	}

	fr_value_box_list_foreach(&in_head->vb_group, vb_in) {
		MEM(vb_out = fr_value_box_alloc_null(ctx));
		if (fr_json_str_from_value(agg, vb_in, quote) < 0) {
			RPERROR("Failed creating escaped JSON value");
			return XLAT_ACTION_FAIL;
		}
		if (fr_value_box_bstrndup(vb_out, vb_out, NULL, fr_sbuff_start(agg), fr_sbuff_used(agg), vb_in->tainted) < 0) {
			RPERROR("Failed assigning escaped JSON value to output box");
			return XLAT_ACTION_FAIL;
		}
		fr_sbuff_reset_talloc(agg);
		fr_dcursor_append(out, vb_out);
	}

	return XLAT_ACTION_DONE;
}
/** Ensure contents are escaped correctly for a JSON document
 *
 * This allows values to be embedded inside JSON strings.
 *
 * @ingroup xlat_functions
 *
 */
static xlat_action_t json_escape_xlat(TALLOC_CTX *ctx, fr_dcursor_t *out,
				      xlat_ctx_t const *xctx,
				      request_t *request, fr_value_box_list_t *in)
{
	return json_escape(ctx, out, xctx, request, in, false);
}

static xlat_arg_parser_t const json_jpath_validate_xlat_arg[] = {
	{ .required = true, .concat = true, .type = FR_TYPE_STRING },
	XLAT_ARG_PARSER_TERMINATOR
};

/** Ensure contents are quoted correctly for a JSON document
 *
 * This emits values with escaping, and appropriate quoting '"' depending on the
 * type of values being produced.  This lets boxed values be inserted directly
 * as table values and array elements, without needing to determine if the
 * expansion needs to be wrapped in quotes.
 *
 * @ingroup xlat_functions
 *
 */
static xlat_action_t json_quote_xlat(TALLOC_CTX *ctx, fr_dcursor_t *out,
				     xlat_ctx_t const *xctx,
				     request_t *request, fr_value_box_list_t *in)
{
	return json_escape(ctx, out, xctx, request, in, true);
}

/** Determine if a jpath expression is valid
 *
 * @ingroup xlat_functions
 *
 * Writes the output (in the format @verbatim<bytes parsed>[:error]@endverbatim).
 */
static xlat_action_t json_jpath_validate_xlat(TALLOC_CTX *ctx, fr_dcursor_t *out,
					 UNUSED xlat_ctx_t const *xctx,
					 request_t *request, fr_value_box_list_t *in)
{
	fr_value_box_t	*path = fr_value_box_list_head(in);
	fr_jpath_node_t *head;
	ssize_t 	slen;
	char 		*jpath_str;
	fr_value_box_t	*vb;

	MEM(vb = fr_value_box_alloc_null(ctx));

	slen = fr_jpath_parse(request, &head, path->vb_strvalue, path->vb_length);
	if (slen <= 0) {
		fr_value_box_asprintf(ctx, vb, NULL, false, "%zu:%s", -(slen), fr_strerror());
		fr_dcursor_append(out, vb);
		fr_assert(head == NULL);
		return XLAT_ACTION_DONE;
	}
	fr_assert(talloc_get_type_abort(head, fr_jpath_node_t));

	jpath_str = fr_jpath_asprint(request, head);

	fr_value_box_asprintf(ctx, vb, NULL, false, "%zu:%s", (size_t) slen, jpath_str);
	fr_dcursor_append(out, vb);
	talloc_free(head);
	talloc_free(jpath_str);

	return XLAT_ACTION_DONE;
}

static xlat_arg_parser_t const json_encode_xlat_arg[] = {
	{ .required = true, .concat = true, .type = FR_TYPE_STRING },
	XLAT_ARG_PARSER_TERMINATOR
};

/** Convert given attributes to a JSON document
 *
 * Usage is `%json.encode(attr tmpl list)`
 *
 * @ingroup xlat_functions
 */
static xlat_action_t json_encode_xlat(TALLOC_CTX *ctx, fr_dcursor_t *out,
				      xlat_ctx_t const *xctx,
				      request_t *request, fr_value_box_list_t *in)
{
	rlm_json_t const	*inst = talloc_get_type_abort_const(xctx->mctx->mi->data, rlm_json_t);
	fr_json_format_t const	*format = inst->format;

	ssize_t			slen;
	tmpl_t			*vpt = NULL;
	fr_pair_list_t		json_vps, vps;
	bool			negate;
	char			*json_str = NULL;
	fr_value_box_t		*vb;
	fr_sbuff_t		sbuff;
	fr_value_box_t		*in_head = fr_value_box_list_head(in);

	fr_pair_list_init(&json_vps);
	fr_pair_list_init(&vps);

	sbuff = FR_SBUFF_IN(in_head->vb_strvalue, in_head->vb_length);
	fr_sbuff_adv_past_whitespace(&sbuff, SIZE_MAX, NULL);

	/*
	 * Iterate through the list of attribute templates in the xlat. For each
	 * one we either add it to the list of attributes for the JSON document
	 * or, if prefixed with '!', remove from the JSON list.
	 */
	while (fr_sbuff_extend(&sbuff)) {
		negate = false;

		/* Check if we should be removing attributes */
		if (fr_sbuff_next_if_char(&sbuff, '!')) negate = true;

		/* Decode next attr template */
		slen = tmpl_afrom_attr_substr(ctx, NULL, &vpt,
					      &sbuff,
					      &json_arg_parse_rules,
					      &(tmpl_rules_t){
					      	.attr = {
							.list_def = request_attr_request,
							.allow_wildcard = true,
							.dict_def = request->proto_dict,
					      	}
					      });
		if (slen <= 0) {
			fr_sbuff_set(&sbuff, (size_t)(slen * -1));
			REMARKER(fr_sbuff_start(&sbuff), fr_sbuff_used(&sbuff), "%s", fr_strerror());
		error:
			fr_pair_list_free(&json_vps);
			talloc_free(vpt);
			return XLAT_ACTION_FAIL;
		}

		/*
		 * Get attributes from the template.
		 * Missing attribute isn't an error (so -1, not 0).
		 */
		if (tmpl_copy_pairs(ctx, &vps, request, vpt) < -1) {
			RPEDEBUG("Error copying attributes");
			goto error;
		}

		if (negate) {
			/* Remove all template attributes from JSON list */
			for (fr_pair_t *vp = fr_pair_list_head(&vps);
			     vp;
			     vp = fr_pair_list_next(&vps, vp)) {

				fr_pair_t *vpm = fr_pair_list_head(&json_vps);
				while (vpm) {
					if (vp->da == vpm->da) {
						fr_pair_t *next = fr_pair_list_next(&json_vps, vpm);
						fr_pair_delete(&json_vps, vpm);
						vpm = next;
						continue;
					}
					vpm = fr_pair_list_next(&json_vps, vpm);
				}
			}

			fr_pair_list_free(&vps);
		} else {
			/* Add template VPs to JSON list */
			fr_pair_list_append(&json_vps, &vps);
		}

		TALLOC_FREE(vpt);

		/* Jump forward to next attr */
		fr_sbuff_adv_past_whitespace(&sbuff, SIZE_MAX, NULL);
	}

	/*
	 * Given the list of attributes we now have in json_vps,
	 * convert them into a JSON document and append it to the
	 * return cursor.
	 */
	MEM(vb = fr_value_box_alloc_null(ctx));

	json_str = fr_json_afrom_pair_list(vb, &json_vps, format);
	if (!json_str) {
		REDEBUG("Failed to generate JSON string");
		goto error;
	}
	fr_value_box_bstrdup_buffer_shallow(NULL, vb, NULL, json_str, false);

	fr_dcursor_append(out, vb);
	fr_pair_list_free(&json_vps);

	return XLAT_ACTION_DONE;
}

/** Pre-parse and validate literal jpath expressions for maps
 *
 * @param[in] cs	#CONF_SECTION that defined the map instance.
 * @param[in] mod_inst	module instance (unused).
 * @param[in] proc_inst	the cache structure to fill.
 * @param[in] src	Where to get the JSON data from.
 * @param[in] maps	set of maps to translate to jpaths.
 * @return
 *	- 0 on success.
 * 	- -1 on failure.
 */
static int mod_map_proc_instantiate(CONF_SECTION *cs, UNUSED void const *mod_inst, void *proc_inst,
				    tmpl_t const *src, map_list_t const *maps)
{
	rlm_json_jpath_cache_t	*cache_inst = proc_inst;
	map_t const		*map = NULL;
	ssize_t			slen;
	rlm_json_jpath_cache_t	*cache = cache_inst, **tail = &cache->next;

	if (!src) {
		cf_log_err(cs, "Missing JSON source");

		return -1;
	}

	while ((map = map_list_next(maps, map))) {
		CONF_PAIR	*cp = cf_item_to_pair(map->ci);
		char const	*p;

#ifndef HAVE_JSON_OBJECT_GET_INT64
		if (tmpl_is_attr(map->lhs) && (tmpl_attr_tail_da(map->lhs)->type == FR_TYPE_UINT64)) {
			cf_log_err(cp, "64bit integers are not supported by linked json-c.  "
				      "Upgrade to json-c >= 0.10 to use this feature");
			return -1;
		}
#endif

		switch (map->rhs->type) {
		case TMPL_TYPE_DATA_UNRESOLVED:
			p = map->rhs->name;
			slen = fr_jpath_parse(cache, &cache->jpath, p, map->rhs->len);
			if (slen <= 0) {
			error:
				cf_canonicalize_error(cp, slen, "Syntax error", fr_strerror());
				return -1;
			}
			break;

		case TMPL_TYPE_DATA:
			if (tmpl_value_type(map->rhs) != FR_TYPE_STRING) {
				cf_log_err(cp, "Right side of map must be a string");
				return -1;
			}
			p = tmpl_value(map->rhs)->vb_strvalue;
			slen = fr_jpath_parse(cache, &cache->jpath, p, tmpl_value_length(map->rhs));
			if (slen <= 0) goto error;
			break;

		default:
			continue;
		}

		/*
		 *	Slightly weird... This is here because our first
		 *	list member was pre-allocated and passed to the
		 *	instantiation callback.
		 */
		if (map_list_next(maps, map)) {
			*tail = cache = talloc_zero(cache, rlm_json_jpath_cache_t);
			tail = &cache->next;
		}
	}

	return 0;
}

/** Converts a string value into a #fr_pair_t
 *
 * @param[in,out] ctx to allocate #fr_pair_t (s).
 * @param[out] out where to write the resulting #fr_pair_t.
 * @param[in] request The current request.
 * @param[in] map to process.
 * @param[in] uctx The json tree/jpath expression to evaluate.
 * @return
 *	- 0 on success.
 *	- -1 on failure.
 */
static int _json_map_proc_get_value(TALLOC_CTX *ctx, fr_pair_list_t *out, request_t *request,
				    map_t const *map, void *uctx)
{
	fr_pair_t			*vp;
	rlm_json_jpath_to_eval_t	*to_eval = uctx;
	fr_value_box_t			*value;
	fr_value_box_list_t		head;
	int				ret;

	fr_pair_list_free(out);
	fr_value_box_list_init(&head);

	ret = fr_jpath_evaluate_leaf(request, &head, tmpl_attr_tail_da(map->lhs)->type, tmpl_attr_tail_da(map->lhs),
			     	     to_eval->root, to_eval->jpath);
	if (ret < 0) {
		RPEDEBUG("Failed evaluating jpath");
		return -1;
	}
	if (ret == 0) return 0;
	fr_assert(!fr_value_box_list_empty(&head));

	for (value = fr_value_box_list_head(&head);
	     value;
	     fr_pair_append(out, vp), value = fr_value_box_list_next(&head, value)) {
		MEM(vp = fr_pair_afrom_da(ctx, tmpl_attr_tail_da(map->lhs)));

		if (fr_value_box_steal(vp, &vp->data, value) < 0) {
			RPEDEBUG("Copying data to attribute failed");
			talloc_free(vp);
			fr_pair_list_free(out);
			return -1;
		}
	}

	return 0;
}

/** Parses a JSON string, and executes jpath queries against it to map values to attributes
 *
 * @param p_result	Result of applying map:
 *			- #RLM_MODULE_NOOP no rows were returned or columns matched.
 *			- #RLM_MODULE_UPDATED if one or more #fr_pair_t were added to the #request_t.
 *			- #RLM_MODULE_FAIL if a fault occurred.
 * @param mpctx		Call context for the map processor, containing the jpath cache.
 * @param request	The current request.
 * @param json		JSON string to parse.
 * @param maps		Head of the map list.
 * @return UNLANG_ACTION_CALCULATE_RESULT
 */
static unlang_action_t mod_map_proc(unlang_result_t *p_result, map_ctx_t const *mpctx, request_t *request,
				    fr_value_box_list_t *json, map_list_t const *maps)
{
	rlm_rcode_t			rcode = RLM_MODULE_UPDATED;
	struct json_tokener		*tok;

	rlm_json_jpath_cache_t const	*cache = mpctx->mpi;
	map_t const			*map = NULL;

	rlm_json_jpath_to_eval_t	to_eval;

	char const			*json_str = NULL;
	fr_value_box_t			*json_head = fr_value_box_list_head(json);

	if (!json_head) {
		REDEBUG("JSON map input cannot be (null)");
		RETURN_UNLANG_FAIL;
	}

	if (fr_value_box_list_concat_in_place(request,
					      json_head, json, FR_TYPE_STRING,
					      FR_VALUE_BOX_LIST_FREE, true,
					      SIZE_MAX) < 0) {
		REDEBUG("Failed concatenating input");
		RETURN_UNLANG_FAIL;
	}
	json_str = json_head->vb_strvalue;

	if ((talloc_array_length(json_str) - 1) == 0) {
		REDEBUG("JSON map input length must be > 0");
		RETURN_UNLANG_FAIL;
	}

	tok = json_tokener_new();
	to_eval.root = json_tokener_parse_ex(tok, json_str, (int)(talloc_array_length(json_str) - 1));
	if (!to_eval.root) {
		REMARKER(json_str, tok->char_offset, "%s", json_tokener_error_desc(json_tokener_get_error(tok)));
		rcode = RLM_MODULE_FAIL;
		goto finish;
	}

	while ((map = map_list_next(maps, map))) {
		switch (map->rhs->type) {
		/*
		 *	Cached types
		 */
		case TMPL_TYPE_DATA_UNRESOLVED:
		case TMPL_TYPE_DATA:
			to_eval.jpath = cache->jpath;

			if (map_to_request(request, map, _json_map_proc_get_value, &to_eval) < 0) {
				rcode = RLM_MODULE_FAIL;
				goto finish;
			}
			cache = cache->next;
			break;

		/*
		 *	Dynamic types
		 */
		default:
		{
			ssize_t		slen;
			fr_jpath_node_t	*node;
			char		*to_parse;

			if (tmpl_aexpand(request, &to_parse, request, map->rhs, fr_jpath_escape_func, NULL) < 0) {
				RPERROR("Failed getting jpath data");
				rcode = RLM_MODULE_FAIL;
				goto finish;
			}
			slen = fr_jpath_parse(request, &node, to_parse, talloc_array_length(to_parse) - 1);
			if (slen <= 0) {
				REMARKER(to_parse, -(slen), "%s", fr_strerror());
				talloc_free(to_parse);
				rcode = RLM_MODULE_FAIL;
				goto finish;
			}
			to_eval.jpath = node;

			if (map_to_request(request, map, _json_map_proc_get_value, &to_eval) < 0) {
				talloc_free(node);
				talloc_free(to_parse);
				rcode = RLM_MODULE_FAIL;
				goto finish;
			}
			talloc_free(node);
		}
			break;
		}
	}


finish:
	json_object_put(to_eval.root);
	json_tokener_free(tok);

	RETURN_UNLANG_RCODE(rcode);
}

static int mod_instantiate(module_inst_ctx_t const *mctx)
{
	rlm_json_t const	*inst = talloc_get_type_abort(mctx->mi->data, rlm_json_t);
	CONF_SECTION		*conf = mctx->mi->conf;
	fr_json_format_t	*format = inst->format;

	/*
	 *	Check the output format type and warn on unused
	 *	format options
	 */
	format->output_mode = fr_table_value_by_str(fr_json_format_table, format->output_mode_str, JSON_MODE_UNSET);
	if (format->output_mode == JSON_MODE_UNSET) {
		cf_log_err(conf, "output_mode value \"%s\" is invalid", format->output_mode_str);
		return -1;
	}
	fr_json_format_verify(format, true);

	return 0;
}

static int mod_bootstrap(module_inst_ctx_t const *mctx)
{
	rlm_json_t const	*inst = talloc_get_type_abort(mctx->mi->data, rlm_json_t);
	xlat_t			*xlat;

	xlat = module_rlm_xlat_register(mctx->mi->boot, mctx, "encode", json_encode_xlat, FR_TYPE_STRING);
	xlat_func_args_set(xlat, json_encode_xlat_arg);

	if (map_proc_register(mctx->mi->boot, inst, "json", mod_map_proc,
			      mod_map_proc_instantiate, sizeof(rlm_json_jpath_cache_t), FR_VALUE_BOX_SAFE_FOR_ANY) < 0) return -1;
	return 0;
}

static int mod_load(void)
{
	xlat_t	*xlat;

	fr_json_version_print();

	if (unlikely(!(xlat = xlat_func_register(NULL, "json.escape", json_escape_xlat, FR_TYPE_STRING)))) return -1;
	xlat_func_args_set(xlat, json_escape_xlat_arg);
	if (unlikely(!(xlat = xlat_func_register(NULL, "json.quote", json_quote_xlat, FR_TYPE_STRING)))) return -1;
	xlat_func_args_set(xlat, json_escape_xlat_arg);
	if (unlikely(!(xlat = xlat_func_register(NULL, "json.jpath_validate", json_jpath_validate_xlat, FR_TYPE_STRING)))) return -1;
	xlat_func_args_set(xlat, json_jpath_validate_xlat_arg);

	return 0;
}

static void mod_unload(void)
{
	xlat_func_unregister("json.escape");
	xlat_func_unregister("json.quote");
	xlat_func_unregister("json.jpath_validate");
}

/*
 *	The module name should be the only globally exported symbol.
 *	That is, everything else should be 'static'.
 *
 *	If the module needs to temporarily modify it's instantiation
 *	data, the type should be changed to MODULE_TYPE_THREAD_UNSAFE.
 *	The server will then take care of ensuring that the module
 *	is single-threaded.
 */
extern module_rlm_t rlm_json;
module_rlm_t rlm_json = {
	.common = {
		.magic		= MODULE_MAGIC_INIT,
		.name		= "json",
		.onload		= mod_load,
		.unload		= mod_unload,
		.config		= module_config,
		.inst_size	= sizeof(rlm_json_t),
		.bootstrap	= mod_bootstrap,
		.instantiate	= mod_instantiate
	}
};

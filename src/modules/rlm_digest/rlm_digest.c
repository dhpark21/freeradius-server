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
 * @file rlm_digest.c
 * @brief Handles SIP digest authentication requests from Cisco SIP servers.
 *
 * @copyright 2002,2006 The FreeRADIUS server project
 * @copyright 2002 Alan DeKok (aland@freeradius.org)
 */
RCSID("$Id$")

#include <freeradius-devel/server/base.h>
#include <freeradius-devel/server/module_rlm.h>

#include <freeradius-devel/util/base16.h>
#include <freeradius-devel/util/md5.h>

typedef struct {
	fr_dict_enum_value_t const	*auth_type;
} rlm_digest_t;

static fr_dict_t const *dict_freeradius;
static fr_dict_t const *dict_radius;

extern fr_dict_autoload_t rlm_digest_dict[];
fr_dict_autoload_t rlm_digest_dict[] = {
	{ .out = &dict_freeradius, .proto = "freeradius" },
	{ .out = &dict_radius, .proto = "radius" },
	{ NULL }
};

static const fr_dict_attr_t *attr_auth_type;
static const fr_dict_attr_t *attr_cleartext_password;

static const fr_dict_attr_t *attr_digest_response;
static const fr_dict_attr_t *attr_digest_attributes;
static const fr_dict_attr_t *attr_digest_algorithm;
static const fr_dict_attr_t *attr_digest_body_digest;
static const fr_dict_attr_t *attr_digest_cnonce;
static const fr_dict_attr_t *attr_digest_ha1;
static const fr_dict_attr_t *attr_digest_method;
static const fr_dict_attr_t *attr_digest_nonce;
static const fr_dict_attr_t *attr_digest_nonce_count;
static const fr_dict_attr_t *attr_digest_qop;
static const fr_dict_attr_t *attr_digest_realm;
static const fr_dict_attr_t *attr_digest_uri;
static const fr_dict_attr_t *attr_digest_user_name;

extern fr_dict_attr_autoload_t rlm_digest_dict_attr[];
fr_dict_attr_autoload_t rlm_digest_dict_attr[] = {
	{ .out = &attr_auth_type, .name = "Auth-Type", .type = FR_TYPE_UINT32, .dict = &dict_freeradius },
	{ .out = &attr_cleartext_password, .name = "Password.Cleartext", .type = FR_TYPE_STRING, .dict = &dict_freeradius },

	{ .out = &attr_digest_response, .name = "Digest-Response", .type = FR_TYPE_STRING, .dict = &dict_radius },
	{ .out = &attr_digest_attributes, .name = "Digest-Attributes", .type = FR_TYPE_TLV, .dict = &dict_radius },
	{ .out = &attr_digest_algorithm, .name = "Digest-Attributes.Algorithm", .type = FR_TYPE_STRING, .dict = &dict_radius },
	{ .out = &attr_digest_body_digest, .name = "Digest-Attributes.Body-Digest", .type = FR_TYPE_STRING, .dict = &dict_radius },
	{ .out = &attr_digest_cnonce, .name = "Digest-Attributes.Cnonce", .type = FR_TYPE_STRING, .dict = &dict_radius },
	{ .out = &attr_digest_ha1, .name = "Digest-Attributes.HA1", .type = FR_TYPE_STRING, .dict = &dict_radius },
	{ .out = &attr_digest_method, .name = "Digest-Attributes.Method", .type = FR_TYPE_STRING, .dict = &dict_radius },
	{ .out = &attr_digest_nonce, .name = "Digest-Attributes.Nonce", .type = FR_TYPE_STRING, .dict = &dict_radius },
	{ .out = &attr_digest_nonce_count, .name = "Digest-Attributes.Nonce-Count", .type = FR_TYPE_STRING, .dict = &dict_radius },
	{ .out = &attr_digest_qop, .name = "Digest-Attributes.Qop", .type = FR_TYPE_STRING, .dict = &dict_radius },
	{ .out = &attr_digest_realm, .name = "Digest-Attributes.Realm", .type = FR_TYPE_STRING, .dict = &dict_radius },
	{ .out = &attr_digest_uri, .name = "Digest-Attributes.Uri", .type = FR_TYPE_STRING, .dict = &dict_radius },
	{ .out = &attr_digest_user_name, .name = "Digest-Attributes.User-Name", .type = FR_TYPE_STRING, .dict = &dict_radius },

	{ NULL }
};

static unlang_action_t CC_HINT(nonnull) mod_authorize(unlang_result_t *p_result, module_ctx_t const *mctx, request_t *request)
{
	rlm_digest_t const	*inst = talloc_get_type_abort(mctx->mi->data, rlm_digest_t);
	fr_pair_t		*vp;

	/*
	 *	Find the first attribute which is parented by Digest-Attributes.
	 */
	vp = fr_pair_find_by_da(&request->request_pairs, NULL, attr_digest_attributes);
	if (!vp) RETURN_UNLANG_NOOP;

	if (!inst->auth_type) {
		WARN("No 'authenticate %s {...}' section or 'Auth-Type = %s' set.  Cannot setup Digest authentication",
		     mctx->mi->name, mctx->mi->name);
		RETURN_UNLANG_NOOP;
	}

	/*
	 *	Everything's OK, add a digest authentication type.
	 */
	if (!module_rlm_section_type_set(request, attr_auth_type, inst->auth_type)) RETURN_UNLANG_NOOP;

	RETURN_UNLANG_OK;
}

/*
 *	Perform all of the wondrous variants of digest authentication.
 */
static unlang_action_t CC_HINT(nonnull) mod_authenticate(unlang_result_t *p_result, UNUSED module_ctx_t const *mctx, request_t *request)
{
	size_t a1_len, a2_len, kd_len;
	uint8_t a1[(FR_MAX_STRING_LEN + 1) * 5]; /* can be 5 attributes */
	uint8_t a2[(FR_MAX_STRING_LEN + 1) * 3]; /* can be 3 attributes */
	uint8_t kd[(FR_MAX_STRING_LEN + 1) * 5];
	uint8_t hash[16];	/* MD5 output */
	fr_pair_t *vp, *passwd, *algo;
	fr_pair_t *qop, *nonce;
	fr_pair_list_t *list;

	/*
	 *	We require access to the plain-text password, or to the
	 *	Digest-Attributes.HA1 parameter.
	 */
	passwd = fr_pair_find_by_da_nested(&request->control_pairs, NULL, attr_digest_ha1);
	if (passwd) {
		if (passwd->vp_length != 32) {
			REDEBUG("Digest-Attributes.HA1 has invalid length, authentication failed");
			RETURN_UNLANG_INVALID;
		}
	} else {
		passwd = fr_pair_find_by_da_nested(&request->control_pairs, NULL, attr_cleartext_password);
	}
	if (!passwd) {
		REDEBUG("Password.Cleartext or Digest-Attributes.HA1 is required for authentication");
		RETURN_UNLANG_INVALID;
	}

	vp = fr_pair_find_by_da(&request->request_pairs, NULL, attr_digest_attributes);
	if (!vp) {
		REDEBUG("Digest-Attributes is required for authentication");
		RETURN_UNLANG_INVALID;
	}
	list = &vp->vp_group;

	/*
	 *	We require access to the Digest-Attributes.Nonce
	 */
	nonce = fr_pair_find_by_da_nested(list, NULL, attr_digest_nonce);
	if (!nonce) {
		REDEBUG("No Digest-Attributes.Nonce: Cannot perform Digest authentication");
		RETURN_UNLANG_INVALID;
	}

	/*
	 *	A1 = Digest-Attributes.User-Name ":" Realm ":" Password
	 */
	vp = fr_pair_find_by_da_nested(list, NULL, attr_digest_user_name);
	if (!vp) {
		REDEBUG("No Digest-Attributes.User-Name: Cannot perform Digest authentication");
		RETURN_UNLANG_INVALID;
	}
	memcpy(&a1[0], vp->vp_octets, vp->vp_length);
	a1_len = vp->vp_length;

	a1[a1_len] = ':';
	a1_len++;

	vp = fr_pair_find_by_da_nested(list, NULL, attr_digest_realm);
	if (!vp) {
		REDEBUG("No Digest-Attributes.Attributes.Realm: Cannot perform Digest authentication");
		RETURN_UNLANG_INVALID;
	}
	memcpy(&a1[a1_len], vp->vp_octets, vp->vp_length);
	a1_len += vp->vp_length;

	a1[a1_len] = ':';
	a1_len++;

	if (passwd->da == attr_cleartext_password) {
		memcpy(&a1[a1_len], passwd->vp_octets, passwd->vp_length);
		a1_len += passwd->vp_length;
		a1[a1_len] = '\0';
		RDEBUG2("A1 = %s", a1);
	} else {
		a1[a1_len] = '\0';
		RDEBUG2("A1 = %s (using Digest-Attributes.HA1)", a1);
		a1_len = 16;
	}

	/*
	 *	See which variant we calculate.
	 *	Assume MD5 if no Digest-Algorithm attribute received
	 */
	algo = fr_pair_find_by_da_nested(list, NULL, attr_digest_algorithm);
	if ((!algo) ||
	    (strcasecmp(algo->vp_strvalue, "MD5") == 0)) {
		/*
		 *	Set A1 to Digest-Attributes.HA1 if no User-Password found
		 */
		if (passwd->da == attr_digest_ha1) {
			if (fr_base16_decode(NULL, &FR_DBUFF_TMP(&a1[0], sizeof(a1)),
				       &FR_SBUFF_IN(passwd->vp_strvalue, passwd->vp_length), false) != 16) {
				RDEBUG2("Invalid text in Digest-Attributes.HA1");
				RETURN_UNLANG_INVALID;
			}
		}

	} else if (strcasecmp(algo->vp_strvalue, "MD5-sess") == 0) {
		/*
		 *	K1 = H(A1) : Digest-Attributes.Nonce ... : H(A2)
		 *
		 *	If we find Digest-Attributes.HA1, we assume it contains
		 *	H(A1).
		 */
		if (passwd->da == attr_cleartext_password) {
			fr_md5_calc(hash, &a1[0], a1_len);
			fr_base16_encode(&FR_SBUFF_OUT((char *) &a1[0], 32 + 1), &FR_DBUFF_TMP(hash, 16));
		} else {	/* MUST be Digest-Attributes.HA1 */
			memcpy(&a1[0], passwd->vp_strvalue, 32);
		}
		a1_len = 32;

		a1[a1_len] = ':';
		a1_len++;

		/*
		 *	Tack on the Digest-Attributes.Nonce. Length must be even
		 */
		if ((nonce->vp_length & 1) != 0) {
			REDEBUG("Received Digest-Attributes.Nonce hex string with invalid length: Cannot perform Digest authentication");
			RETURN_UNLANG_INVALID;
		}
		memcpy(&a1[a1_len], nonce->vp_octets, nonce->vp_length);
		a1_len += nonce->vp_length;

		a1[a1_len] = ':';
		a1_len++;

		vp = fr_pair_find_by_da_nested(list, NULL, attr_digest_cnonce);
		if (!vp) {
			REDEBUG("No Digest-Attributes.CNonce: Cannot perform Digest authentication");
			RETURN_UNLANG_INVALID;
		}

		/*
		 *      Digest-Attributes.CNonce length must be even
		 */
		if ((vp->vp_length & 1) != 0) {
			REDEBUG("Received Digest-Attributes.CNonce hex string with invalid length: Cannot perform Digest authentication");
			RETURN_UNLANG_INVALID;
		}
		memcpy(&a1[a1_len], vp->vp_octets, vp->vp_length);
		a1_len += vp->vp_length;

	} else if (strcasecmp(algo->vp_strvalue, "MD5") != 0) {
		/*
		 *	We check for "MD5-sess" and "MD5".
		 *	Anything else is an error.
		 */
		REDEBUG("%pP - Unknown Digest-Attributes.Algorithm: Cannot perform Digest authentication", vp);
		RETURN_UNLANG_INVALID;
	}

	/*
	 *	A2 = Digest-Attributes.Method ":" Digest-Attributes.URI
	 */
	vp = fr_pair_find_by_da_nested(list, NULL, attr_digest_method);
	if (!vp) {
		REDEBUG("No Digest-Attributes.Method: Cannot perform Digest authentication");
		RETURN_UNLANG_INVALID;
	}
	memcpy(&a2[0], vp->vp_octets, vp->vp_length);
	a2_len = vp->vp_length;

	a2[a2_len] = ':';
	a2_len++;

	vp = fr_pair_find_by_da_nested(list, NULL, attr_digest_uri);
	if (!vp) {
		REDEBUG("No Digest-Attributes.URI: Cannot perform Digest authentication");
		RETURN_UNLANG_INVALID;
	}
	memcpy(&a2[a2_len], vp->vp_octets, vp->vp_length);
	a2_len += vp->vp_length;

	/*
	 *  QOP is "auth-int", tack on ": Digest-Attributes.Body-Digest"
	 */
	qop = fr_pair_find_by_da_nested(list, NULL, attr_digest_qop);
	if (qop) {
		if (strcasecmp(qop->vp_strvalue, "auth-int") == 0) {
			fr_pair_t *body;

			/*
			 *	Add in Digest-Attributes.Body-Digest
			 */
			a2[a2_len] = ':';
			a2_len++;

			/*
			 *  Must be a hex representation of an MD5 digest.
			 */
			body = fr_pair_find_by_da_nested(list, NULL, attr_digest_body_digest);
			if (!body) {
				REDEBUG("No Digest-Attributes.Body-Digest: Cannot perform Digest authentication");
				RETURN_UNLANG_INVALID;
			}

			if ((a2_len + body->vp_length) > sizeof(a2)) {
				REDEBUG("Digest-Attributes.Body-Digest is too long");
				RETURN_UNLANG_INVALID;
			}

			memcpy(a2 + a2_len, body->vp_octets, body->vp_length);
			a2_len += body->vp_length;

		} else if (strcasecmp(qop->vp_strvalue, "auth") != 0) {
			REDEBUG("%pP - Unknown value: Cannot perform Digest authentication", qop);
			RETURN_UNLANG_INVALID;
		}
	}

	a2[a2_len] = '\0';
	RDEBUG2("A2 = %s", a2);

	/*
	 *     KD = H(A1) : Digest-Attributes.Nonce ... : H(A2).
	 *     Compute MD5 if Digest-Attributes.Algorithm == "MD5-Sess",
	 *     or if we found a User-Password.
	 */
	if (((algo != NULL) && (strcasecmp(algo->vp_strvalue, "MD5-Sess") == 0)) ||
	    (passwd->da == attr_cleartext_password)) {
		a1[a1_len] = '\0';
		fr_md5_calc(&hash[0], &a1[0], a1_len);
	} else {
		memcpy(&hash[0], &a1[0], a1_len);
	}
	fr_base16_encode(&FR_SBUFF_OUT((char *) kd, (sizeof(hash) * 2) + 1), &FR_DBUFF_TMP(hash, sizeof(hash)));

	RHEXDUMP_INLINE3(hash, sizeof(hash), "H(A1)");

	kd_len = 32;

	kd[kd_len] = ':';
	kd_len++;

	memcpy(&kd[kd_len], nonce->vp_octets, nonce->vp_length);
	kd_len += nonce->vp_length;

	/*
	 *	No QOP defined.  Do RFC 2069 compatibility.
	 */
	if (!qop) {
		/*
		 *	Do nothing here.
		 */

	} else {		/* Digest-Attributes.QOP MUST be "auth" or "auth-int" */
		/*
		 *	Tack on ":" Digest-Attributes.Nonce-Count ":" Digest-Attributes.CNonce
		 *	       ":" Digest-Attributes.QOP
		 */
		kd[kd_len] = ':';
		kd_len++;

		vp = fr_pair_find_by_da_nested(list, NULL, attr_digest_nonce_count);
		if (!vp) {
			REDEBUG("No Digest-Attributes.Nonce-Count: Cannot perform Digest authentication");
			RETURN_UNLANG_INVALID;
		}
		memcpy(&kd[kd_len], vp->vp_octets, vp->vp_length);
		kd_len += vp->vp_length;

		kd[kd_len] = ':';
		kd_len++;

		vp = fr_pair_find_by_da_nested(list, NULL, attr_digest_cnonce);
		if (!vp) {
			REDEBUG("No Digest-Attributes.CNonce: Cannot perform Digest authentication");
			RETURN_UNLANG_INVALID;
		}
		memcpy(&kd[kd_len], vp->vp_octets, vp->vp_length);
		kd_len += vp->vp_length;

		kd[kd_len] = ':';
		kd_len++;

		memcpy(&kd[kd_len], qop->vp_octets, qop->vp_length);
		kd_len += qop->vp_length;
	}

	/*
	 *	Tack on ":" H(A2)
	 */
	kd[kd_len] = ':';
	kd_len++;

	fr_md5_calc(&hash[0], &a2[0], a2_len);

	fr_base16_encode(&FR_SBUFF_OUT((char *) kd + kd_len, (sizeof(hash) * 2) + 1), &FR_DBUFF_TMP(hash, sizeof(hash)));

	RHEXDUMP_INLINE3(hash, sizeof(hash), "H(A2)");

	kd_len += 32;

	kd[kd_len] = 0;

	RDEBUG2("KD = %s\n", &kd[0]);

	/*
	 *	Take the hash of KD.
	 */
	fr_md5_calc(&hash[0], &kd[0], kd_len);
	memcpy(&kd[0], &hash[0], 16);

	/*
	 *	Get the binary value of Digest-Response.  This isn't
	 *	inside of the Digest-Attributes group.
	 */
	vp = fr_pair_find_by_da(&request->request_pairs, NULL, attr_digest_response);
	if (!vp) {
		REDEBUG("No Digest-Response attribute in the request.  Cannot perform digest authentication");
		RETURN_UNLANG_INVALID;
	}

	if (fr_base16_decode(NULL, &FR_DBUFF_TMP(&hash[0], sizeof(hash)),
		       &FR_SBUFF_IN(vp->vp_strvalue, vp->vp_length), false) != (ssize_t)(vp->vp_length >> 1)) {
		RDEBUG2("Invalid text in Digest-Response");
		RETURN_UNLANG_INVALID;
	}

	RDEBUG3("Comparing hashes, received: %pV, calculated: %pH", &vp->data, fr_box_octets(kd, 16));

	/*
	 *  And finally, compare the digest in the packet with KD.
	 */
	if (memcmp(&kd[0], &hash[0], 16) == 0) RETURN_UNLANG_OK;

	REDEBUG("FAILED authentication");
	RETURN_UNLANG_REJECT;
}


/*
 *	Create instance for our module. Allocate space for
 *	instance structure and read configuration parameters
 */
static int mod_instantiate(module_inst_ctx_t const *mctx)
{
	rlm_digest_t		*inst = talloc_get_type_abort(mctx->mi->data, rlm_digest_t);

	inst->auth_type = fr_dict_enum_by_name(attr_auth_type, mctx->mi->name, -1);
	if (!inst->auth_type) {
		WARN("Failed to find 'authenticate %s {...}' section.  Digest authentication will likely not work",
		     mctx->mi->name);
	}

	return 0;
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
extern module_rlm_t rlm_digest;
module_rlm_t rlm_digest = {
	.common = {
		.magic		= MODULE_MAGIC_INIT,
		.name		= "digest",
		.inst_size	= sizeof(rlm_digest_t),
		.instantiate	= mod_instantiate,
	},
	.method_group = {
		.bindings = (module_method_binding_t[]){
			{ .section = SECTION_NAME("authenticate", CF_IDENT_ANY), .method = mod_authenticate },
			{ .section = SECTION_NAME("recv", "Access-Request"), .method = mod_authorize },
			MODULE_BINDING_TERMINATOR
		}
	}
};

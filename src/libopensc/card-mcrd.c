/*
 * card-mcrd.c: Support for MICARDO cards
 *
 * Copyright (C) 2004  Martin Paljak <martin@paljak.pri.ee>
 * Copyright (C) 2004  Priit Randla <priit.randla@eyp.ee>
 * Copyright (C) 2003  Marie Fischer <marie@vtl.ee> 
 * Copyright (C) 2001  Juha Yrj�l� <juha.yrjola@iki.fi>
 * Copyright (C) 2002  g10 Code GmbH
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 2.1 of the License, or (at your option) any later version.
 *
 * This library is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public
 * License along with this library; if not, write to the Free Software
 * Foundation, Inc., 59 Temple Place, Suite 330, Boston, MA  02111-1307  USA
 */

#include "internal.h"
#include "asn1.h"
#include "cardctl.h"
#include <stdlib.h>
#include <string.h>
#include <ctype.h>
#include "esteid.h"

#define TYPE_UNKNOWN	0
#define TYPE_ANY	1
#define TYPE_ESTEID	2

static struct sc_atr_table_hex mcrd_atrs[] = {
	{ "3B:FF:94:00:FF:80:B1:FE:45:1F:03:00:68:D2:76:00:00:28:FF:05:1E:31:80:00:90:00:23", "German BMI", TYPE_ANY },
	{ "3B:FE:94:00:FF:80:B1:FA:45:1F:03:45:73:74:45:49:44:20:76:65:72:20:31:2E:30:43", "EstEID (cold)", TYPE_ESTEID },
	{ "3B:6E:00:FF:45:73:74:45:49:44:20:76:65:72:20:31:2E:30", "EstEID (warm)", TYPE_ESTEID },
	{ NULL }
};

static struct sc_card_operations mcrd_ops;
static struct sc_card_driver mcrd_drv = {
	"MICARDO 2.1",
	"mcrd",
	&mcrd_ops
};

static const struct sc_card_operations *iso_ops = NULL;

enum {
	MCRD_SEL_MF  = 0x00,
	MCRD_SEL_DF  = 0x01,
	MCRD_SEL_EF  = 0x02,
	MCRD_SEL_AID = 0x04
};

#define MFID 0x3F00
#define EF_KeyD 0x0013  /* File with extra key information. */
#define EF_Rule 0x0030  /* Default ACL file. */

#define MAX_CURPATH 10 

struct rule_record_s {
	struct rule_record_s *next;
	int recno;
	size_t datalen;
	u8 data[1];
};

struct keyd_record_s {
	struct keyd_record_s *next;
	int recno;
	size_t datalen;
	u8 data[1];
};

struct df_info_s {
	struct df_info_s *next;
	unsigned short path[MAX_CURPATH];
	size_t pathlen; 
	struct rule_record_s *rule_file; /* keeps records of EF_Rule. */
	struct keyd_record_s *keyd_file; /* keeps records of EF_KeyD. */
};

struct mcrd_priv_data {
	unsigned short curpath[MAX_CURPATH]; /* The currently selected path. */
	size_t curpathlen; /* Length of this path or 0 if unknown. */
	int is_ef;      /* True if the path points to an EF. */
	struct df_info_s *df_infos; 
	sc_security_env_t sec_env;	/* current security environment */
};

#define DRVDATA(card)        ((struct mcrd_priv_data *) ((card)->drv_data))

static int load_special_files(struct sc_card *card);
static int select_part (struct sc_card *card, u8 kind, unsigned short int fid,
	                struct sc_file **file);

/* Return the DF_info for the current path.  If does not yet exist,
   create it.  Returns NULL on error. */
static struct df_info_s *get_df_info (struct sc_card *card)
{
	struct sc_context *ctx = card->ctx;
	struct mcrd_priv_data *priv = DRVDATA (card);
	struct df_info_s *dfi;

	assert (!priv->is_ef);

	if (!priv->curpathlen) {
		sc_debug(ctx, "no current path to find the df_info\n");
		return NULL;
	}
		
	for (dfi = priv->df_infos; dfi; dfi = dfi->next) {
		if (dfi->pathlen == priv->curpathlen
		    && !memcmp (dfi->path, priv->curpath,
				dfi->pathlen *sizeof *dfi->path))
			return dfi;
	}
	/* Not found, create it. */
	dfi = (struct df_info_s *) calloc (1, sizeof *dfi);
	if (!dfi) {
		sc_debug(ctx, "out of memory while allocating df_info\n");
		return NULL;
	}
	dfi->pathlen = priv->curpathlen;
	memcpy (dfi->path, priv->curpath, dfi->pathlen * sizeof *dfi->path);
	dfi->next = priv->df_infos;
	priv->df_infos = dfi;
	return dfi;
}

static void clear_special_files (struct df_info_s *dfi)
{
	if (dfi) {
		while (dfi->rule_file) {
			struct rule_record_s *tmp = dfi->rule_file->next;
			free (dfi->rule_file);
			dfi->rule_file = tmp;
		}
		while (dfi->keyd_file) {
			struct keyd_record_s *tmp = dfi->keyd_file->next;
			free (dfi->keyd_file);
			dfi->keyd_file = tmp;
		}
	}
}

/* Some functionality straight from the EstEID manual. 
 * Official notice: Refer to the Micardo 2.1 Public manual.
 * Sad side: not available without a NDA.
 */
 
static int
mcrd_delete_ref_to_authkey (struct sc_card *card)
{
	struct sc_apdu apdu;
	int r;
	u8 sbuf[SC_MAX_APDU_BUFFER_SIZE];
	
	assert (card != NULL);
	sc_format_apdu (card, &apdu, SC_APDU_CASE_3_SHORT, 0x22, 0x41, 0xA4);

	sbuf[0] = 0x83;
	sbuf[1] = 0x00;
	apdu.data = sbuf;
	apdu.lc = 2;
	apdu.datalen = 2;
	r = sc_transmit_apdu (card, &apdu);
	SC_TEST_RET (card->ctx, r, "APDU transmit failed");
	SC_FUNC_RETURN (card->ctx, 2, sc_check_sw (card, apdu.sw1, apdu.sw2));
}

static int
mcrd_delete_ref_to_signkey (struct sc_card *card)
{
	struct sc_apdu apdu;
	int r;
	u8 sbuf[SC_MAX_APDU_BUFFER_SIZE];
	assert (card != NULL);

	sc_format_apdu (card, &apdu, SC_APDU_CASE_3_SHORT, 0x22, 0x41, 0xB6);

	sbuf[0] = 0x83;
	sbuf[1] = 0x00;
	apdu.data = sbuf;
	apdu.lc = 2;
	apdu.datalen = 2;
	r = sc_transmit_apdu (card, &apdu);
	SC_TEST_RET (card->ctx, r, "APDU transmit failed");
	SC_FUNC_RETURN (card->ctx, 2, sc_check_sw (card, apdu.sw1, apdu.sw2));

}

static int
mcrd_set_decipher_key_ref (struct sc_card *card, int key_reference)
{
	struct sc_apdu apdu;
	struct sc_path path;
	int r;
	u8 sbuf[SC_MAX_APDU_BUFFER_SIZE];
	u8 keyref_data[SC_ESTEID_KEYREF_FILE_RECLEN];
	assert (card != NULL);

	sc_format_apdu (card, &apdu, SC_APDU_CASE_3_SHORT, 0x22, 0x41, 0xB8);   
	/* track the active keypair  */
	sc_format_path("0033", &path);
	r = sc_select_file(card, &path, NULL);
	SC_TEST_RET(card->ctx, r,
		    "Can't select keyref info file 0x0033");
	r = sc_read_record(card, 1, keyref_data,
			   SC_ESTEID_KEYREF_FILE_RECLEN,
			   SC_RECORD_BY_REC_NR);
	SC_TEST_RET(card->ctx, r,
		    "Can't read keyref info file!");

	sc_debug(card->ctx,
		 "authkey reference 0x%02x%02x\n", 
		  keyref_data[9], keyref_data[10]);

	sc_debug(card->ctx,
		 "signkey reference 0x%02x%02x\n", 
		  keyref_data[19], keyref_data[20]);		  


	sbuf[0] = 0x83;
	sbuf[1] = 0x03;
	sbuf[2] = 0x80;
	switch (key_reference) {
		case 1:
			sbuf[3] = keyref_data[9];
			sbuf[4] = keyref_data[10];
			break;
		case 2:
			sbuf[3] = keyref_data[19];
			sbuf[4] = keyref_data[20];
			break;
	}
	apdu.data = sbuf;
	apdu.lc = 5;
	apdu.datalen = 5;
	r = sc_transmit_apdu (card, &apdu);
	SC_TEST_RET (card->ctx, r, "APDU transmit failed");
	SC_FUNC_RETURN (card->ctx, 2, sc_check_sw (card, apdu.sw1, apdu.sw2));
}

static int sc_card_type(struct sc_card *card)
{
	int i, type;

	i = _sc_match_atr_hex(card, mcrd_atrs, &type);
	if (i < 0)
		return 0;
	return type;
}

static int mcrd_match_card(struct sc_card *card)
{
	int i;

	i = _sc_match_atr_hex(card, mcrd_atrs, NULL);
	if (i < 0)
		return 0;
	return 1;
}

static int mcrd_init(struct sc_card *card)
{
	unsigned long flags;
	struct mcrd_priv_data *priv;

	priv = (struct mcrd_priv_data *) calloc (1, sizeof *priv);
	if (!priv)
		return SC_ERROR_OUT_OF_MEMORY;
	card->name = "MICARDO 2.1";
	card->drv_data = priv;
	card->cla = 0x00;

	flags = SC_ALGORITHM_RSA_RAW;
	flags |= SC_ALGORITHM_RSA_PAD_PKCS1;
	flags |= SC_ALGORITHM_RSA_HASH_NONE;

	_sc_card_add_rsa_alg(card, 512, flags, 0);
	_sc_card_add_rsa_alg(card, 768, flags, 0);
	_sc_card_add_rsa_alg(card, 1024, flags, 0);

	priv->curpath[0] = MFID;
	priv->curpathlen = 1;
	if (sc_card_type(card) != TYPE_ESTEID)
		load_special_files (card);
	return 0;
}

static int mcrd_finish(struct sc_card *card)
{
	struct mcrd_priv_data *priv;

	if (card == NULL)
		return 0;
	priv = DRVDATA (card);
	while (priv->df_infos) {
		struct df_info_s *tmp = priv->df_infos->next;
		clear_special_files (priv->df_infos);
		priv->df_infos = tmp;
	}
	free(priv);
	return 0;
}


/* Load the rule and keyd file into our private data.
   Return 0 on success */
static int load_special_files (struct sc_card *card)
{
	struct sc_context *ctx = card->ctx;
	struct mcrd_priv_data *priv = DRVDATA (card);
	int r, recno;
	struct df_info_s *dfi;
	struct rule_record_s *rule;
	struct keyd_record_s *keyd;

	assert (!priv->is_ef);

	/* First check whether we already cached it. */
	dfi = get_df_info (card);
	if (dfi && dfi->rule_file) 
		return 0; /* yes. */
	clear_special_files (dfi);

	/* Read rule file. Note that we bypass our cache here. */
	r = select_part (card, 2, EF_Rule, NULL);
	SC_TEST_RET(ctx, r, "selecting EF_Rule failed");

	for (recno=1;; recno++) {
		struct sc_apdu apdu;
		u8 recvbuf[200];

		sc_format_apdu(card, &apdu, SC_APDU_CASE_2_SHORT,
			       0xB2, recno, 0x04);
		apdu.le      = sizeof recvbuf;
		apdu.resplen = sizeof recvbuf;
		apdu.resp = recvbuf;
		r = sc_transmit_apdu(card, &apdu);
		SC_TEST_RET(card->ctx, r, "APDU transmit failed");
		if (apdu.sw1 == 0x6a && apdu.sw2 == 0x83)
			break; /* No more records. */
		if (!((apdu.sw1 == 0x90 && apdu.sw2 == 0x00)
		      ||(apdu.sw1 == 0x62 && apdu.sw2 == 0x82)))
			SC_FUNC_RETURN(ctx, 2,
				       sc_check_sw(card, apdu.sw1, apdu.sw2));
		rule = (struct rule_record_s *) malloc (sizeof*rule + apdu.resplen);
		if (!rule)
			SC_FUNC_RETURN(ctx, 0, SC_ERROR_OUT_OF_MEMORY);
		rule->recno = recno;
		rule->datalen = apdu.resplen;
		memcpy (rule->data, apdu.resp, apdu.resplen);
		rule->next = dfi->rule_file;
		dfi->rule_file = rule;
	}

	sc_debug(ctx, "new EF_Rule file loaded (%d records)\n", recno-1);

	/* Read the KeyD file. Note that we bypass our cache here. */
	r = select_part (card, 2, EF_KeyD, NULL);
	if (r == SC_ERROR_FILE_NOT_FOUND) {
		sc_debug(ctx, "no EF_KeyD file available\n");
		return 0; /* That is okay. */
	}
	SC_TEST_RET(ctx, r, "selecting EF_KeyD failed");

	for (recno=1;; recno++) {
		struct sc_apdu apdu;
		u8 recvbuf[200];

		sc_format_apdu(card, &apdu, SC_APDU_CASE_2_SHORT,
			       0xB2, recno, 0x04);
		apdu.le      = sizeof recvbuf;
		apdu.resplen = sizeof recvbuf;
		apdu.resp = recvbuf;
		r = sc_transmit_apdu(card, &apdu);
		SC_TEST_RET(card->ctx, r, "APDU transmit failed");
		if (apdu.sw1 == 0x6a && apdu.sw2 == 0x83)
			break; /* No more records. */
		if (!((apdu.sw1 == 0x90 && apdu.sw2 == 0x00)
		      ||(apdu.sw1 == 0x62 && apdu.sw2 == 0x82)))
			SC_FUNC_RETURN(ctx, 2,
				       sc_check_sw(card, apdu.sw1, apdu.sw2));
		keyd = (struct keyd_record_s *) malloc (sizeof *keyd + apdu.resplen);
		if (!keyd)
			SC_FUNC_RETURN(ctx, 0, SC_ERROR_OUT_OF_MEMORY);
		keyd->recno = recno;
		keyd->datalen = apdu.resplen;
		memcpy (keyd->data, apdu.resp, apdu.resplen);
		keyd->next = dfi->keyd_file;
		dfi->keyd_file = keyd;
	}

	sc_debug(ctx, "new EF_KeyD file loaded (%d records)\n", recno-1);
	/* fixme: Do we need to restore the current DF?  I guess it is
	   not required, but we could try to do so by selecting 3fff?  */
	return 0;
}



/* Return the SE number from the keyD for the FID.  If ref_data is not
   NULL the reference data is returned; this shoudl be an array of at
   least 2 bytes.  Returns -1 on error.  */
static int get_se_num_from_keyd (struct sc_card *card, unsigned short fid,
	                         u8 *ref_data)
{
	struct sc_context *ctx = card->ctx;
	struct df_info_s *dfi;
	struct keyd_record_s *keyd;
	size_t len, taglen;
	const u8 *p, *tag;
	char dbgbuf[2048];
	u8 fidbuf[2];

	fidbuf[0] = fid >> 8;
	fidbuf[1] = fid;

	dfi = get_df_info (card);
	if (!dfi || !dfi->keyd_file) {
		sc_debug (ctx, "EF_keyD not loaded\n");
		return -1;
	}    

	for (keyd=dfi->keyd_file; keyd; keyd = keyd->next) {
		p = keyd->data;
		len = keyd->datalen;

		sc_hex_dump (ctx, p,len, dbgbuf, sizeof dbgbuf);
		sc_debug (ctx, "keyd no %d:\n%s", keyd->recno, dbgbuf);
		
		tag = sc_asn1_find_tag(ctx, p, len, 0x83, &taglen);
		if (!tag || taglen != 4 ||
		    !(tag[2] == fidbuf[0] && tag[3] == fidbuf[1]))
			continue;
		/* Found a matching record. */
		if (ref_data) {
			ref_data[0] = tag[0];
			ref_data[1] = tag[1];
		}
		/* Look for the SE-DO */
		tag = sc_asn1_find_tag(ctx, p, len, 0x7B, &taglen);
		if (!tag || !taglen)
			continue;
		p = tag;
		len = taglen;
		/* And now look for the referenced SE. */
		tag = sc_asn1_find_tag(ctx, p, len, 0x80, &taglen);
		if (!tag || taglen != 1)
			continue;
		return *tag; /* found. */
	}
	sc_debug (ctx, "EF_keyD for %04hx not found\n", fid);
	return -1;
}

/* Process an ARR (7816-9/8.5.4) and setup the ACL. */
static void process_arr(struct sc_card *card, struct sc_file *file,
	                const u8 *buf, size_t buflen)
{
	struct sc_context *ctx = card->ctx;
	struct df_info_s *dfi;
	struct rule_record_s *rule;
	size_t left, taglen;
	unsigned int cla, tag;
	const u8 *p;
	int skip;
	char dbgbuf[2048];

	/* Currently we support only the short for. */
	if (buflen != 1) {
		sc_debug (ctx, "can't handle long ARRs\n");
		return;
	}

	dfi = get_df_info (card);
	for (rule = dfi? dfi->rule_file:NULL; rule && rule->recno != *buf;
	     rule = rule->next)
		;
	if (!rule) {
		sc_debug (ctx, "referenced EF_rule record %d not found\n", *buf);
		return;
	}

	if (ctx->debug) {
	  sc_hex_dump (ctx, rule->data, rule->datalen, dbgbuf, sizeof dbgbuf);
	  sc_debug (ctx, "rule for record %d:\n%s", *buf, dbgbuf);
	}

	p = rule->data;
	left = rule->datalen;
	skip = 1; /* Skip over initial unknown SC DOs. */
	for (;;) {
		buf = p;
		if (sc_asn1_read_tag(&p, left, &cla, &tag, &taglen) != 1)
			break;
		left -= (p - buf);
		tag |= cla;
		
		if (tag == 0x80 && taglen != 1) {
			skip = 1;
		}
		else if (tag == 0x80) { /* AM byte. */
			sc_debug (ctx,"  AM_DO: %02x\n", *p);
			skip = 0;
		}
		else if (tag >= 0x81 && tag <= 0x8f) {/* Cmd description */
			sc_hex_dump (ctx, p, taglen, dbgbuf, sizeof dbgbuf);
			sc_debug (ctx, "  AM_DO: cmd[%s%s%s%s] %s",
			       (tag & 8)? "C":"",
			       (tag & 4)? "I":"",
			       (tag & 2)? "1":"",
			       (tag & 1)? "2":"",
			       dbgbuf);
			skip = 0;
		}
		else if (tag == 0x9C) {/* Proprietary state machine descrip.*/
			skip = 1;
		}
		else if (!skip) {
			sc_hex_dump (ctx, p, taglen, dbgbuf, sizeof dbgbuf);
			switch (tag) {
			case 0x90: /* Always */
				sc_debug (ctx,"     SC: always\n");
				break;
			case 0x97: /* Never */
				sc_debug (ctx,"     SC: never\n");
				break;
			case 0xA4: /* Authentication, value is a CRT. */
				sc_debug (ctx,"     SC: auth %s", dbgbuf);
				break;
				
			case 0xB4:
			case 0xB6:
			case 0xB8: /* Cmd or resp with SM, value is a CRT. */
				sc_debug (ctx,"     SC: cmd/resp %s", dbgbuf);
				break;
				
			case 0x9E: /* Security Condition byte. */
				sc_debug (ctx,"     SC: condition %s", dbgbuf);
				break;

			case 0xA0: /* OR template. */
				sc_debug (ctx,"     SC: OR\n");
				break;
			case 0xAF: /* AND template. */
				sc_debug (ctx,"     SC: AND\n");
				break;
			}
		}
		left -= taglen;
		p += taglen;
	}

}


static void process_fcp(struct sc_card *card, struct sc_file *file,
	                     const u8 *buf, size_t buflen)
{
	struct sc_context *ctx = card->ctx;
	size_t taglen, len = buflen;
	const u8 *tag = NULL, *p = buf;
	int bad_fde = 0;

	if (ctx->debug >= 3)
		sc_debug(ctx, "processing FCI bytes\n");
	/* File identifier. */
	tag = sc_asn1_find_tag(ctx, p, len, 0x83, &taglen);
	if (tag != NULL && taglen == 2) {
		file->id = (tag[0] << 8) | tag[1];
		if (ctx->debug >= 3)
			sc_debug(ctx, "  file identifier: 0x%02X%02X\n", tag[0],
			       tag[1]);
	}
	/* Number of data bytes in the file including structural information.*/
	tag = sc_asn1_find_tag(ctx, p, len, 0x81, &taglen);
	if (!tag) {
		/* My card does not encode the filelength in 0x81 but
		   in 0x85 which is the file descriptor extension in TCOS.
		   Assume that this is the case when the regular file
		   size tag is not encoded. */
		tag = sc_asn1_find_tag(ctx, p, len, 0x85, &taglen);
		bad_fde = !!tag;
	}
	if (tag != NULL && taglen >= 2) {
		int bytes = (tag[0] << 8) + tag[1];
		if (ctx->debug >= 3)
			sc_debug(ctx, "  bytes in file: %d\n", bytes);
		file->size = bytes;
	}
	if (tag == NULL) {
		tag = sc_asn1_find_tag(ctx, p, len, 0x80, &taglen);
		if (tag != NULL && taglen >= 2) {
			int bytes = (tag[0] << 8) + tag[1];
			if (ctx->debug >= 3)
				sc_debug(ctx, "  bytes in file: %d\n", bytes);
			file->size = bytes;
		}
	}

	/* File descriptor byte(s). */
	tag = sc_asn1_find_tag(ctx, p, len, 0x82, &taglen);
	if (tag != NULL) {
		/* Fixme, this might actual be up to 6 bytes. */
		if (taglen > 0) {
			unsigned char byte = tag[0];
			const char *type;

			file->shareable = byte & 0x40 ? 1 : 0;
			if (ctx->debug >= 3)
				sc_debug(ctx, "  shareable: %s\n",
				       (byte & 0x40) ? "yes" : "no");
			file->ef_structure = byte & 0x07;
			switch ((byte >> 3) & 7) {
			case 0:
				type = "working EF";
				file->type = SC_FILE_TYPE_WORKING_EF;
				break;
			case 1:
				type = "internal EF";
				file->type = SC_FILE_TYPE_INTERNAL_EF;
				break;
			case 7:
				type = "DF";
				file->type = SC_FILE_TYPE_DF;
				break;
			default:
				type = "unknown";
				break;
			}
			if (ctx->debug >= 3) {
				sc_debug(ctx, "  type: %s\n", type);
				sc_debug(ctx, "  EF structure: %d\n",
				       byte & 0x07);
			}
		}
	}

	/* DF name. */
	tag = sc_asn1_find_tag(ctx, p, len, 0x84, &taglen);
	if (tag != NULL && taglen > 0 && taglen <= 16) {
		char name[17];
		size_t i;

		memcpy(file->name, tag, taglen);
		file->namelen = taglen;

		for (i = 0; i < taglen; i++) {
			if (isalnum(tag[i]) || ispunct(tag[i])
			    || isspace(tag[i]))
				name[i] = tag[i];
			else
				name[i] = '?';
		}
		name[taglen] = 0;
		if (ctx->debug >= 3)
			sc_debug(ctx, "  file name: %s\n", name);
	}

	/* Proprietary information. */
	tag = bad_fde? NULL : sc_asn1_find_tag(ctx, p, len, 0x85, &taglen);
	if (tag != NULL && taglen) {
		sc_file_set_prop_attr(file, tag, taglen); 
	} else
		file->prop_attr_len = 0;

	/* Proprietary information, constructed. */
	tag = sc_asn1_find_tag(ctx, p, len, 0xA5, &taglen);
	if (tag != NULL && taglen) {
		sc_file_set_prop_attr(file, tag, taglen); 
	}

	/* Security attributes, proprietary format. */
	tag = sc_asn1_find_tag(ctx, p, len, 0x86, &taglen);
	if (tag != NULL && taglen) {
		sc_file_set_sec_attr(file, tag, taglen); 
	}

	/* Security attributes, reference to expanded format. */
	tag = sc_asn1_find_tag(ctx, p, len, 0x8B, &taglen);
	if (tag && taglen) {
		process_arr (card, file, tag, taglen);
	}
	else if ((tag = sc_asn1_find_tag(ctx, p, len, 0xA1, &taglen))
		 && taglen) {
		/* Not found, but there is a Security Attribute
		   Template for interface mode. */
		tag = sc_asn1_find_tag(ctx, tag, taglen, 0x8B, &taglen);
		if (tag && taglen)
			process_arr (card, file, tag, taglen);
	}
	
	file->magic = SC_FILE_MAGIC;
}

/* Send a select command and parse the response. */
static int
do_select(struct sc_card *card, u8 kind,
	  const u8 *buf, size_t buflen,
	  struct sc_file **file)
{
	struct sc_apdu	apdu;
	u8	resbuf[SC_MAX_APDU_BUFFER_SIZE];
	int r;

	/* create the apdu */
	memset(&apdu, 0, sizeof(apdu));
	apdu.cla = 0x00;
	apdu.cse = SC_APDU_CASE_3_SHORT;
	apdu.ins = 0xA4;
	apdu.p1 = kind;
	apdu.p2 = 0;
	apdu.data = buf;
	apdu.datalen = buflen;
	apdu.lc = apdu.datalen;
	apdu.resp = resbuf;
	apdu.resplen = file ? sizeof(resbuf) : 0;

	r = sc_transmit_apdu(card, &apdu);
	SC_TEST_RET(card->ctx, r, "APDU transmit failed");
	if (!file) {
		if (apdu.sw1 == 0x61)
				SC_FUNC_RETURN(card->ctx, 2, 0);
		r = sc_check_sw(card, apdu.sw1, apdu.sw2);
		if (!r && kind == MCRD_SEL_AID) 
				card->cache.current_path.len = 0;
		SC_FUNC_RETURN(card->ctx, 2, r);
	}
	r = sc_check_sw(card, apdu.sw1, apdu.sw2);
	if (r)
		SC_FUNC_RETURN(card->ctx, 2, r);

	switch (apdu.resp[0]) {
	case 0x6F:
		*file = sc_file_new();
		if (!*file)
			SC_FUNC_RETURN(card->ctx, 0, SC_ERROR_OUT_OF_MEMORY);
		if (apdu.resp[1] <= apdu.resplen)
			process_fcp (card, *file, apdu.resp+2, apdu.resp[1]);
		break;
	case 0x00:	/* proprietary coding */
		SC_FUNC_RETURN(card->ctx, 2, SC_ERROR_UNKNOWN_DATA_RECEIVED);
	default:
		SC_FUNC_RETURN(card->ctx, 2, SC_ERROR_UNKNOWN_DATA_RECEIVED);
	}
	return 0;
}

/* Wrapper around do_select to be used when multiple selects are
   required. */
static int
select_part (struct sc_card *card, u8 kind, unsigned short int fid,
	     struct sc_file **file)
{
	u8 fbuf[2];
	int r;

	if (card->ctx->debug >=3)
		sc_debug(card->ctx, "select_part (0x%04X, kind=%u)\n",
		      fid, kind);
	
	if (fid == MFID)
		kind = MCRD_SEL_MF; /* force this kind. */

	fbuf[0] = fid >> 8;
	fbuf[1] = fid & 0xff;
	card->ctx->suppress_errors++;
	r = do_select (card, kind, fbuf, 2, file);
	card->ctx->suppress_errors--;

	return r;
}


/* Select a file by iterating over the FID in the PATHPTR array while
   updating the curpath kept in the private data cache.  With DF_ONLY
   passed as true only DF are selected, otherwise the function tries
   to figure out whether the last path item is a DF or EF. */
static int
select_down (struct sc_card *card,
	     unsigned short *pathptr, size_t pathlen,
	     int df_only,
	     struct sc_file **file)
{
	struct mcrd_priv_data *priv = DRVDATA (card);
	int r;
	int found_ef = 0;

	if (!pathlen)
		return SC_ERROR_INVALID_ARGUMENTS; 

	for (; pathlen; pathlen--, pathptr++) {
		if (priv->curpathlen == MAX_CURPATH)
			SC_TEST_RET (card->ctx, SC_ERROR_INTERNAL,
				     "path too long for cache");
		r = -1; /* force DF select. */
		if (pathlen == 1 && !df_only) {
			/* first try to select an EF and retry an DF
			   on error. */
			r = select_part (card, MCRD_SEL_EF,*pathptr, file);
			if (!r) 
				found_ef = 1;
		}
		if (r)
			r = select_part (card, MCRD_SEL_DF, *pathptr,
					     pathlen == 1? file : NULL);
		SC_TEST_RET(card->ctx, r, "unable to select DF");
		priv->curpath[priv->curpathlen] = *pathptr;
		priv->curpathlen++;
	}
	priv->is_ef = found_ef;
	if (!found_ef) 
		load_special_files (card);
	
	return 0;
}

/* Handle the selection case when a PATH is requested.  Our card does
   not support this addressing so we have to emulate it.  To keep the
   security status we should not unnecessary change the directory;
   this is accomplished be keeping track of the currently selected
   file.  Note that PATH is an array of PATHLEN file ids and not the
   usual sc_path structure. */
   
static int
select_file_by_path (struct sc_card *card, unsigned short *pathptr,
	             size_t pathlen,
	             struct sc_file **file)
{
	struct mcrd_priv_data *priv = DRVDATA (card);
	int r;
	size_t i;
	
	assert (!priv->curpathlen || priv->curpath[0] == MFID);
	
	if (pathlen && *pathptr == 0x3FFF) {
		pathlen--;
		pathptr++;
	}

	if (!pathlen || pathlen >= MAX_CURPATH)
		r = SC_ERROR_INVALID_ARGUMENTS;
	else if (pathlen == 1 && pathptr[0] == MFID) {
		/* MF requested: clear the cache and select it. */
		priv->curpathlen = 0;
		r = select_part (card, MCRD_SEL_MF, pathptr[0], file);
		SC_TEST_RET(card->ctx, r, "unable to select MF");
		priv->curpath[0] = pathptr[0];
		priv->curpathlen = 1;
		priv->is_ef = 0;
	}
	else if (pathlen > 1 && pathptr[0] == MFID) {
		/* Absolute addressing, check cache to avoid
		   unnecessary selects. */
		for (i=0; (i < pathlen && i < priv->curpathlen
				&& pathptr[i] == priv->curpath[i]); i++)
				;
		if (!priv->curpathlen) {
			/* Need to do all selects starting at the root. */
			priv->curpathlen = 0;
			priv->is_ef = 0;
			r = select_down (card, pathptr, pathlen, 0, file);
		} else if ( i==pathlen && i < priv->curpathlen) {
			/* Go upwards; we do it the easy way and start
			   at the root.  However we know that the target is a DF. */
			priv->curpathlen = 0;
			priv->is_ef = 0;
			r = select_down (card, pathptr, pathlen, 1, file);
		} else if (i == pathlen && i == priv->curpathlen) {
			/* Already selected. */
			if (!file)
					r=0; /* The caller did not request the fci. */
			else {
				/* This EF or DF was already selected, but
				   we need to get the FCI, so we have
				to select again. */
				assert (priv->curpathlen > 1);
				priv->curpathlen--;
				priv->is_ef = 0;
				r = select_down (card, pathptr+pathlen-1, 1, 0, file);
			}
		} else {
			/* We have to append something.  For now we
			   simply start at the root. (fixme) */
			priv->curpathlen = 0;
			priv->is_ef = 0;
			r = select_down (card, pathptr, pathlen, 0, file);
		} 
	} else {
		/* Relative addressing. */
		if (!priv->curpathlen) {
			/* Relative addressing without a current path. So we
			   select the MF first. */
			r = select_part (card, MCRD_SEL_MF, pathptr[0], file);
			SC_TEST_RET(card->ctx, r, "unable to select MF");
			priv->curpath[0] = pathptr[0];
			priv->curpathlen = 1;
			priv->is_ef = 0;
		}
		if (priv->is_ef) {
			assert (priv->curpathlen > 1);
			priv->curpathlen--;
			priv->is_ef = 0;
		}
		r = select_down (card, pathptr, pathlen, 0, file);
	}		
	return r;
}

static int
select_file_by_fid (struct sc_card *card, unsigned short *pathptr,
	            size_t pathlen, struct sc_file **file)
{
	struct mcrd_priv_data *priv = DRVDATA (card);
	int r;

	assert (!priv->curpathlen || priv->curpath[0] == MFID);
	
	if (pathlen > 1)
		return SC_ERROR_INVALID_ARGUMENTS;

	if (pathlen && *pathptr == 0x3FFF) 
		return 0;

	if (!pathlen) {
		/* re-select the current one if needed. */
		if (!file)
			r=0; /* The caller did not request the fci. */
		else if (!priv->curpathlen) {
			/* There is no current file. */
			r = SC_ERROR_INTERNAL;
		}
		else {
			assert (priv->curpathlen > 1);
			priv->curpathlen--;
			priv->is_ef = 0;
			r = select_down (card, pathptr, 1, 0, file);
		}
	}
	else if (pathptr[0] == MFID) {
		/* MF requested: clear the cache and select it. */
		priv->curpathlen = 0;
		r = select_part (card, MCRD_SEL_MF, MFID, file);
		SC_TEST_RET(card->ctx, r, "unable to select MF");
		priv->curpath[0] = MFID;
		priv->curpathlen = 1;
		priv->is_ef = 0;
	}
	else {
		/* Relative addressing. */
		if (!priv->curpathlen) {
			/* Relative addressing without a current path. So we
			   select the MF first. */
			r = select_part (card, MCRD_SEL_MF,
					 pathptr[0], file);
			SC_TEST_RET(card->ctx, r, "unable to select MF");
			priv->curpath[0] = pathptr[0];
			priv->curpathlen = 1;
			priv->is_ef = 0;
		}
		if (priv->is_ef) {
			assert (priv->curpathlen > 1);
			priv->curpathlen--;
			priv->is_ef = 0;
		}
		r = select_down (card, pathptr, 1, 0, file);
	}

	return r;
}


/* This drivers select command handler. */
static int
mcrd_select_file(struct sc_card *card, const struct sc_path *path,
	         struct sc_file **file)
{
	struct mcrd_priv_data *priv = DRVDATA (card);
	int r = 0;

	SC_FUNC_CALLED(card->ctx, 1);
	
	if (card->ctx->debug >= 3) {
		char line[256], *linep = line;
		size_t i;

		linep += sprintf(linep, "requesting type %d, path ", path->type);
		for (i = 0; i < path->len; i++) {
			sprintf(linep, "%02X", path->value[i]);
			linep += 2;
		}
		strcpy(linep, "\n");
		sc_debug(card->ctx, line);

		linep = line;
		linep += sprintf(linep, "ef=%d, curpath=",priv->is_ef);
		
		for (i=0; i < priv->curpathlen; i++) {
			sprintf(linep, "%04X", priv->curpath[i]);
			linep += 4;
		}
		strcpy(linep, "\n");
		sc_debug(card->ctx, line);
	}

	if (path->type == SC_PATH_TYPE_DF_NAME) {
		if (path->len > 16)
			return SC_ERROR_INVALID_ARGUMENTS;
		r = do_select(card, MCRD_SEL_AID, path->value, path->len, file);
		priv->curpathlen = 0;
	} else {
		unsigned short int pathtmp[SC_MAX_PATH_SIZE/2];
		unsigned short int *pathptr;
		
		size_t pathlen, n;
		if ((path->len & 1) || path->len > sizeof(pathtmp))
			return SC_ERROR_INVALID_ARGUMENTS;
			
		pathptr = pathtmp;
		for (n = 0; n < path->len; n += 2)
			pathptr[n>>1] = (path->value[n] << 8)|path->value[n+1];
		pathlen = path->len >> 1;
		if (path->type == SC_PATH_TYPE_PATH) 
			r = select_file_by_path (card, pathptr, pathlen, file);
		else {  /* SC_PATH_TYPE_FILEID */
			r = select_file_by_fid (card, pathptr, pathlen, file);
		}
	}

	if (card->ctx->debug >= 3) {
		char line[256], *linep = line;
		size_t i;
		linep += sprintf(linep, "  result=%d, ef=%d, curpath=", r, priv->is_ef);
		for (i=0; i < priv->curpathlen; i++) {
			sprintf(linep, "%04X", priv->curpath[i]);
			linep += 4;
		}	
	strcpy(linep, "\n");
	sc_debug(card->ctx, line);
	}
	return r;
}


/* Crypto operations */

static int mcrd_enable_se (struct sc_card *card, int se_num)
{
	struct sc_apdu apdu;
	int r;

	sc_format_apdu(card, &apdu, SC_APDU_CASE_1, 0x22, 0xF3, se_num);
	r = sc_transmit_apdu(card, &apdu);
	SC_TEST_RET(card->ctx, r, "APDU transmit failed");
	return sc_check_sw(card, apdu.sw1, apdu.sw2);
}



/* It seems that MICARDO does not fully comply with ISO, so I use
   values gathered from peeking actual signing opeations using a
   different system. 
   It has been generalized [?] and modified by information coming from
   openpgp card implementation, EstEID 'manual' and some other sources. -mp
   */
static int mcrd_set_security_env(struct sc_card *card,
	                         const struct sc_security_env *env,
	                         int se_num)
{
	struct mcrd_priv_data *priv = DRVDATA(card);
	struct sc_apdu apdu;
	u8 sbuf[SC_MAX_APDU_BUFFER_SIZE];
	u8 *p;
	int r, locked = 0;

	assert(card != NULL && env != NULL);
	SC_FUNC_CALLED(card->ctx, 2);
	
	/* special environemnt handling for esteid, stolen from openpgp */
	if (sc_card_type(card) == TYPE_ESTEID) {
		/* some sanity checks */
		if (env->flags & SC_SEC_ENV_ALG_PRESENT) {
			if (env->algorithm != SC_ALGORITHM_RSA)
				return SC_ERROR_INVALID_ARGUMENTS;
		}
		if (!(env->flags & SC_SEC_ENV_KEY_REF_PRESENT)
		    || env->key_ref_len != 1)
			return SC_ERROR_INVALID_ARGUMENTS;

		select_esteid_df(card);	/* is it needed? */
		switch (env->operation) {
		case SC_SEC_OPERATION_DECIPHER:
			sc_debug(card->ctx,
				 "Using keyref %d to dechiper\n",
				 env->key_ref[0]);
			mcrd_enable_se(card, 6);
			mcrd_delete_ref_to_authkey(card);
			mcrd_delete_ref_to_signkey(card);
			mcrd_set_decipher_key_ref(card, env->key_ref[0]);
			break;
		case SC_SEC_OPERATION_SIGN:
			sc_debug(card->ctx, "Using keyref %d to sign\n",
				 env->key_ref[0]);
			mcrd_enable_se(card, 1);
			break;
		default:
			return SC_ERROR_INVALID_ARGUMENTS;
		}
		priv->sec_env = *env;
		return 0;
	}

	sc_format_apdu(card, &apdu, SC_APDU_CASE_3_SHORT, 0x22, 0, 0);
	apdu.le = 0;
	p = sbuf;
	switch (env->operation) {
	case SC_SEC_OPERATION_DECIPHER:
		apdu.p1 = 0x41;
		apdu.p2 = 0xB8;
		break;
	case SC_SEC_OPERATION_SIGN:
		apdu.p1 = 0x41;
		apdu.p2 = 0xB6;
		break;
	default:
		return SC_ERROR_INVALID_ARGUMENTS;
	}
	*p++ = 0x83;
	*p++ = 0x03;
	*p++ = 0x80;
	if ((env->flags & SC_SEC_ENV_FILE_REF_PRESENT)
	    && env->file_ref.len > 1) {
		unsigned short fid;
		int num;

		fid  = env->file_ref.value[env->file_ref.len-2] << 8;
		fid |= env->file_ref.value[env->file_ref.len-1];
		num = get_se_num_from_keyd (card, fid, p);
		if (num != -1) {
			/* Need to restore the security environmnet. */
			if (num) {
				r = mcrd_enable_se (card, num);
				SC_TEST_RET(card->ctx, r, "mcrd_enable_se failed");
			}
			p += 2;
		}
	}
	else {
		return SC_ERROR_INVALID_ARGUMENTS;
	}

	r = p - sbuf;
	apdu.lc = r;
	apdu.datalen = r;
	apdu.data = sbuf;
	apdu.resplen = 0;
	if (se_num > 0) {
		r = sc_lock(card);
		SC_TEST_RET(card->ctx, r, "sc_lock() failed");
		locked = 1;
	}
	if (apdu.datalen != 0) {
		r = sc_transmit_apdu(card, &apdu);
		if (r) {
			sc_perror(card->ctx, r, "APDU transmit failed");
			goto err;
		}
		r = sc_check_sw(card, apdu.sw1, apdu.sw2);
		if (r) {
			sc_perror(card->ctx, r, "Card returned error");
			goto err;
		}
	}
	if (se_num <= 0)
		return 0;
	sc_unlock(card);
	SC_TEST_RET(card->ctx, r, "APDU transmit failed");
	return sc_check_sw(card, apdu.sw1, apdu.sw2);
err:
	if (locked)
		sc_unlock(card);
	return r;
}

/* heavily modified by -mp */
static int mcrd_compute_signature(struct sc_card *card,
                                  const u8 * data, size_t datalen,
                                  u8 * out, size_t outlen)
{
	struct mcrd_priv_data *priv = DRVDATA(card);
	sc_security_env_t *env = &priv->sec_env;
	int r;
	struct sc_apdu apdu;

	assert(card != NULL && data != NULL && out != NULL);
	SC_FUNC_CALLED(card->ctx, 2);
	if (env->operation != SC_SEC_OPERATION_SIGN)
		return SC_ERROR_INVALID_ARGUMENTS;
	if (datalen > 255)
		SC_FUNC_RETURN(card->ctx, 4, SC_ERROR_INVALID_ARGUMENTS);

	sc_debug(card->ctx,
		 "Will compute signature for %d (0x%02x) bytes using key %d\n",
		 datalen, datalen, env->key_ref[0]);

	switch (env->key_ref[0]) {
	case SC_ESTEID_AUTH:	/* authentication key */
		sc_format_apdu(card, &apdu, SC_APDU_CASE_4_SHORT,
			       0x88, 0, 0);
		break;
	default:
		sc_format_apdu(card, &apdu, SC_APDU_CASE_4_SHORT,
			       0x2A, 0x9E, 0x9A);

	}
	apdu.lc = datalen;
	apdu.data = data;
	apdu.datalen = datalen;
	apdu.le = 0x80;
	apdu.resp = out;
	apdu.resplen = outlen;

	r = sc_transmit_apdu(card, &apdu);
	SC_TEST_RET(card->ctx, r, "APDU transmit failed");
	r = sc_check_sw(card, apdu.sw1, apdu.sw2);
	SC_TEST_RET(card->ctx, r, "Card returned error");

	SC_FUNC_RETURN(card->ctx, 4, apdu.resplen);
}

/* added by -mp */
static int mcrd_decipher(struct sc_card *card,
		  const u8 * crgram, size_t crgram_len, u8 * out,
		  size_t out_len)
{

	int r;
	struct sc_apdu apdu;
	struct mcrd_priv_data *priv = DRVDATA(card);
	sc_security_env_t *env = &priv->sec_env;
	u8 *temp;

	sc_debug(card->ctx,
		 "Will dechiper %d (0x%02x) bytes using key %d\n",
		 crgram_len, crgram_len, env->key_ref[0]);

	/* saniti check */
	if (env->operation != SC_SEC_OPERATION_DECIPHER)
		return SC_ERROR_INVALID_ARGUMENTS;

	if (!(temp = (u8 *) malloc(crgram_len + 1)))
		return SC_ERROR_OUT_OF_MEMORY;
	temp[0] = '\0';
	memcpy(temp + 1, crgram, crgram_len);
	crgram = temp;
	crgram_len += 1;

	sc_format_apdu(card, &apdu, SC_APDU_CASE_3_SHORT, 0x2A, 0x80,
		       0x86);

	apdu.resp = out;
	apdu.resplen = out_len;

	apdu.data = crgram;
	apdu.datalen = crgram_len;

	apdu.lc = crgram_len;
	apdu.sensitive = 1;
	r = sc_transmit_apdu(card, &apdu);
	SC_TEST_RET(card->ctx, r, "APDU transmit failed");
	r = sc_check_sw(card, apdu.sw1, apdu.sw2);
	SC_TEST_RET(card->ctx, r, "Card returned error");

	SC_FUNC_RETURN(card->ctx, 4, apdu.resplen);
}

/* added by -mp, to give pin information in the card driver (pkcs15emu->driver needed) */
static int mcrd_pin_cmd(struct sc_card *card, struct sc_pin_cmd_data *data,
		 int *tries_left)
{
	SC_FUNC_CALLED(card->ctx, 3); 
	data->pin1.offset = 5;
	data->pin1.length_offset = 4;
	data->pin2.offset = 5;
	data->pin2.length_offset = 4;
	SC_FUNC_RETURN(card->ctx, 4, iso_ops->pin_cmd(card, data, tries_left));
}

/* Driver binding */
static struct sc_card_driver * sc_get_driver(void)
{
	struct sc_card_driver *iso_drv = sc_get_iso7816_driver();
	if (iso_ops == NULL)
			iso_ops = iso_drv->ops;
	
	mcrd_ops = *iso_drv->ops;
	mcrd_ops.match_card = mcrd_match_card;
	mcrd_ops.init = mcrd_init;
	mcrd_ops.finish = mcrd_finish;
	mcrd_ops.select_file = mcrd_select_file;
	mcrd_ops.set_security_env = mcrd_set_security_env;
	mcrd_ops.compute_signature = mcrd_compute_signature;
	mcrd_ops.decipher = mcrd_decipher;
	mcrd_ops.pin_cmd = mcrd_pin_cmd;
	
	return &mcrd_drv;
}

struct sc_card_driver * sc_get_mcrd_driver(void)
{
	return sc_get_driver();
}

/*
 * $Id$
 *
 * Server-side module for Wiki rooms.  This will handle things like version control. 
 * 
 * Copyright (c) 2009-2009 by the citadel.org team
 *
 *  This program is free software; you can redistribute it and/or modify
 *  it under the terms of the GNU General Public License as published by
 *  the Free Software Foundation; either version 3 of the License, or
 *  (at your option) any later version.
 *
 *  This program is distributed in the hope that it will be useful,
 *  but WITHOUT ANY WARRANTY; without even the implied warranty of
 *  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *  GNU General Public License for more details.
 *
 *  You should have received a copy of the GNU General Public License
 *  along with this program; if not, write to the Free Software
 *  Foundation, Inc., 59 Temple Place, Suite 330, Boston, MA  02111-1307  USA
 */

#include "sysdep.h"
#include <stdlib.h>
#include <unistd.h>
#include <stdio.h>
#include <fcntl.h>
#include <signal.h>
#include <pwd.h>
#include <errno.h>
#include <ctype.h>
#include <sys/types.h>

#if TIME_WITH_SYS_TIME
# include <sys/time.h>
# include <time.h>
#else
# if HAVE_SYS_TIME_H
#  include <sys/time.h>
# else
#  include <time.h>
# endif
#endif

#include <sys/wait.h>
#include <string.h>
#include <limits.h>
#include <libcitadel.h>
#include "citadel.h"
#include "server.h"
#include "citserver.h"
#include "support.h"
#include "config.h"
#include "control.h"
#include "room_ops.h"
#include "user_ops.h"
#include "policy.h"
#include "database.h"
#include "msgbase.h"
#include "euidindex.h"
#include "ctdl_module.h"

/*
 * Before allowing a wiki page save to execute, we have to perform version control.
 * This involves fetching the old version of the page if it exists... FIXME finish this
 */
int wiki_upload_beforesave(struct CtdlMessage *msg) {
	struct CitContext *CCC = CC;
	long old_msgnum = (-1L);
	struct CtdlMessage *old_msg = NULL;
	long history_msgnum = (-1L);
	struct CtdlMessage *history_msg = NULL;
	char diff_old_filename[PATH_MAX];
	char diff_new_filename[PATH_MAX];
	char diff_cmd[PATH_MAX];
	FILE *fp;
	int rv;
	char history_page[1024];
	char boundary[256];
	char endary[260];
	char buf[1024];
	int nbytes = 0;
	char *diffbuf = NULL;
	size_t diffbuf_len = 0;
	char *ptr = NULL;

	if (!CCC->logged_in) return(0);	/* Only do this if logged in. */

	/* Is this a room with a Wiki in it, don't run this hook. */
	if (CCC->room.QRdefaultview != VIEW_WIKI) {
		return(0);
	}

	/* If this isn't a MIME message, don't bother. */
	if (msg->cm_format_type != 4) return(0);

	/* If there's no EUID we can't do this. */
	if (msg->cm_fields['E'] == NULL) return(0);

	/* If there's no message text, obviously this is all b0rken and shouldn't happen at all */
	if (msg->cm_fields['M'] == NULL) return(0);

	/* See if we can retrieve the previous version. */
	old_msgnum = locate_message_by_euid(msg->cm_fields['E'], &CCC->room);
	if (old_msgnum <= 0L) return(0);
	snprintf(history_page, sizeof history_page, "%s_HISTORY_", msg->cm_fields['E']);

	old_msg = CtdlFetchMessage(old_msgnum, 1);
	if (old_msg == NULL) return(0);

	if (old_msg->cm_fields['M'] == NULL) {		/* old version is corrupt? */
		CtdlFreeMessage(old_msg);
		return(0);
	}

	/* If no changes were made, don't bother saving it again */
	if (!strcmp(msg->cm_fields['M'], old_msg->cm_fields['M'])) {
		CtdlFreeMessage(old_msg);
		return(1);
	}

	/*
	 * Generate diffs
	 */
	CtdlMakeTempFileName(diff_old_filename, sizeof diff_old_filename);
	CtdlMakeTempFileName(diff_new_filename, sizeof diff_new_filename);

	fp = fopen(diff_old_filename, "w");
	rv = fwrite(old_msg->cm_fields['M'], strlen(old_msg->cm_fields['M']), 1, fp);
	fclose(fp);
	CtdlFreeMessage(old_msg);

	fp = fopen(diff_new_filename, "w");
	rv = fwrite(msg->cm_fields['M'], strlen(msg->cm_fields['M']), 1, fp);
	fclose(fp);

	diffbuf_len = 0;
	diffbuf = NULL;
	snprintf(diff_cmd, sizeof diff_cmd, "diff -u %s %s", diff_old_filename, diff_new_filename);
	fp = popen(diff_cmd, "r");
	if (fp != NULL) {
		do {
			diffbuf = realloc(diffbuf, diffbuf_len + 1025);
			nbytes = fread(&diffbuf[diffbuf_len], 1, 1024, fp);
			diffbuf_len += nbytes;
		} while (nbytes == 1024);
		diffbuf[diffbuf_len] = 0;
		pclose(fp);
	}
	CtdlLogPrintf(CTDL_DEBUG, "diff length is %d bytes\n", diffbuf_len);

	unlink(diff_old_filename);
	unlink(diff_new_filename);

	/* Determine whether this was a bogus (empty) edit */
	if ((diffbuf_len = 0) && (diffbuf != NULL)) {
		free(diffbuf);
		diffbuf = NULL;
	}
	if (diffbuf == NULL) {
		return(1);		/* No changes at all?  Abandon the post entirely! */
	}

	/* Now look for the existing edit history */

	history_msgnum = locate_message_by_euid(history_page, &CCC->room);
	history_msg = NULL;
	if (history_msgnum > 0L) {
		history_msg = CtdlFetchMessage(old_msgnum, 1);
	}

	/* Create a new history message if necessary */
	if (history_msg == NULL) {
		history_msg = malloc(sizeof(struct CtdlMessage));
		memset(history_msg, 0, sizeof(struct CtdlMessage));
		history_msg->cm_magic = CTDLMESSAGE_MAGIC;
		history_msg->cm_anon_type = MES_NORMAL;
		history_msg->cm_format_type = FMT_RFC822;
		history_msg->cm_fields['A'] = strdup("Citadel");
		history_msg->cm_fields['R'] = strdup(CCC->room.QRname);
		snprintf(boundary, sizeof boundary, "Citadel--Multipart--%04x--%08lx", getpid(), time(NULL));
		history_msg->cm_fields['M'] = malloc(1024);
		snprintf(history_msg->cm_fields['M'], 1024,
			"Content-type: multipart/mixed; boundary=\"%s\"\n\n"
			"This is a Citadel wiki history encoded as multipart MIME.\n"
			"--%s--\n"
			,
			boundary, boundary
		);
	}

	/* Update the history message (regardless of whether it's new or existing) */


	/* First, figure out the boundary string.  We do this even when we generated the
	 * boundary string in the above code, just to be safe and consistent.
	 */
	strcpy(boundary, "");

	ptr = history_msg->cm_fields['M'];
	do {
		ptr = memreadline(ptr, buf, sizeof buf);
		if (*ptr != 0) {
			striplt(buf);
			if (!IsEmptyStr(buf) && (!strncasecmp(buf, "Content-type:", 13))) {
				if (
					(bmstrcasestr(buf, "multipart") != NULL)
					&& (bmstrcasestr(buf, "boundary=") != NULL)
				) {
					safestrncpy(boundary, bmstrcasestr(buf, "\""), sizeof boundary);
					char *qu;
					qu = strchr(boundary, '\"');
					if (qu) {
						strcpy(boundary, ++qu);
					}
					qu = strchr(boundary, '\"');
					if (qu) {
						*qu = 0;
					}
				}
			}
		}
	} while ( (IsEmptyStr(boundary)) && (*ptr != 0) );

	if (!IsEmptyStr(boundary)) {
		snprintf(endary, sizeof endary, "--%s--", boundary);
		history_msg->cm_fields['M'] = realloc(history_msg->cm_fields['M'],
			strlen(history_msg->cm_fields['M']) + strlen(diffbuf) + 512
		);
		ptr = bmstrcasestr(history_msg->cm_fields['M'], endary);
		if (ptr != NULL) {
			sprintf(ptr, "--%s\n"
					"Content-type: text/plain\n"
					"From: %s <%s>\n"
					"\n"
					"%s\n"
					"--%s--\n"
					,
				boundary,
				CCC->user.fullname,
				CCC->cs_inet_email,
				diffbuf,
				boundary
			);
		}

		history_msg->cm_fields['T'] = realloc(history_msg->cm_fields['T'], 32);
		snprintf(history_msg->cm_fields['T'], 32, "%ld", time(NULL));
	
		CtdlLogPrintf(CTDL_DEBUG, "\033[31m%s\033[0m", history_msg->cm_fields['M']);
		/* FIXME now do something with it */
	}

	free(diffbuf);
	free(history_msg);
	return(0);
}


/*
 * Module initialization
 */
CTDL_MODULE_INIT(wiki)
{
	if (!threading)
	{
		CtdlRegisterMessageHook(wiki_upload_beforesave, EVT_BEFORESAVE);
	}

	/* return our Subversion id for the Log */
	return "$Id$";
}
/*
 * $Id$ 
 *
 * Handle XMPP presence exchanges
 *
 * Copyright (c) 2007-2010 by Art Cancro
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 3 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software
 * Foundation, Inc., 59 Temple Place, Suite 330, Boston, MA  02111-1307  USA
 *
 */

#include "sysdep.h"
#include <stdlib.h>
#include <unistd.h>
#include <stdio.h>
#include <fcntl.h>
#include <signal.h>
#include <pwd.h>
#include <errno.h>
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
#include <ctype.h>
#include <expat.h>
#include <libcitadel.h>
#include "citadel.h"
#include "server.h"
#include "citserver.h"
#include "support.h"
#include "config.h"
#include "internet_addressing.h"
#include "md5.h"
#include "ctdl_module.h"
#include "serv_xmpp.h"



/* 
 * Indicate the presence of another user to the client
 * (used in several places)
 */
void xmpp_indicate_presence(char *presence_jid)
{
	cprintf("<presence from=\"%s\" to=\"%s\"></presence>",
		presence_jid,
		XMPP->client_jid
	);
}


/* 
 * Initial dump of the entire wholist
 */
void xmpp_wholist_presence_dump(void)
{
	struct CitContext *cptr = NULL;
	int nContexts, i;
	
	int aide = (CC->user.axlevel >= AxAideU);

	cptr = CtdlGetContextArray(&nContexts);
	if (!cptr) {
		return;
	}

	for (i=0; i<nContexts; i++) {
		if (cptr[i].logged_in) {
			if (
				(((cptr[i].cs_flags&CS_STEALTH)==0) || (aide))	/* aides see everyone */
				&& (cptr[i].user.usernum != CC->user.usernum)	/* don't show myself */
				&& (cptr[i].can_receive_im)			/* IM-capable session */
			) {
				xmpp_indicate_presence(cptr[i].cs_inet_email);
			}
		}
	}
	free(cptr);
}


/*
 * Function to remove a buddy subscription and delete from the roster
 * (used in several places)
 */
void xmpp_destroy_buddy(char *presence_jid) {
	static int unsolicited_id = 1;

	/* Transmit non-presence information */
	cprintf("<presence type=\"unavailable\" from=\"%s\" to=\"%s\"></presence>",
		presence_jid, XMPP->client_jid
	);
	cprintf("<presence type=\"unsubscribed\" from=\"%s\" to=\"%s\"></presence>",
		presence_jid, XMPP->client_jid
	);
	// FIXME ... we should implement xmpp_indicate_nonpresence so we can use it elsewhere

	/* Do an unsolicited roster update that deletes the contact. */
	cprintf("<iq from=\"%s\" to=\"%s\" id=\"unbuddy_%x\" type=\"result\">",
		CC->cs_inet_email,
		XMPP->client_jid,
		++unsolicited_id
	);
	cprintf("<query xmlns=\"jabber:iq:roster\">");
	cprintf("<item jid=\"%s\" subscription=\"remove\">", presence_jid);
	cprintf("<group>%s</group>", config.c_humannode);
	cprintf("</item>");
	cprintf("</query>"
		"</iq>"
	);
}


/*
 * When a user logs in or out of the local Citadel system, notify all XMPP sessions about it.
 */
void xmpp_presence_notify(char *presence_jid, int event_type) {
	struct CitContext *cptr;
	static int unsolicited_id;
	int visible_sessions = 0;
	int nContexts, i;
	int aide = (CC->user.axlevel >= AxAideU);

	if (IsEmptyStr(presence_jid)) return;
	if (CC->kill_me) return;

	cptr = CtdlGetContextArray(&nContexts);
	if (!cptr) {
		return;
	}
		
	/* Count the visible sessions for this user */
	for (i=0; i<nContexts; i++) {
		if (cptr[i].logged_in) {
			if (
				(!strcasecmp(cptr[i].cs_inet_email, presence_jid)) 
				&& (((cptr[i].cs_flags&CS_STEALTH)==0) || (aide))
				&& (cptr[i].can_receive_im)
			) {
				++visible_sessions;
			}
		}
	}

	CtdlLogPrintf(CTDL_DEBUG, "%d sessions for <%s> are now visible to session %d\n",
		visible_sessions, presence_jid, CC->cs_pid);

	if ( (event_type == XMPP_EVT_LOGIN) && (visible_sessions == 1) ) {

		CtdlLogPrintf(CTDL_DEBUG, "Telling session %d that <%s> logged in\n", CC->cs_pid, presence_jid);

		/* Do an unsolicited roster update that adds a new contact. */
		for (i=0; i<nContexts; i++) {
			if (cptr[i].logged_in) {
				if (!strcasecmp(cptr[i].cs_inet_email, presence_jid)) {
					cprintf("<iq id=\"unsolicited_%x\" type=\"result\">",
						++unsolicited_id);
					cprintf("<query xmlns=\"jabber:iq:roster\">");
					xmpp_roster_item(&cptr[i]);
					cprintf("</query>"
						"</iq>");
				}
			}
		}

		/* Transmit presence information */
		xmpp_indicate_presence(presence_jid);
	}

	if (visible_sessions == 0) {
		CtdlLogPrintf(CTDL_DEBUG, "Telling session %d that <%s> logged out\n", CC->cs_pid, presence_jid);

		xmpp_destroy_buddy(presence_jid);
	}
	free(cptr);
}



void xmpp_fetch_mortuary_backend(long msgnum, void *userdata) {
	HashList *mortuary = (HashList *) userdata;
	struct CtdlMessage *msg;
	char *ptr = NULL;
	char *lasts = NULL;

	msg = CtdlFetchMessage(msgnum, 1);
	if (msg == NULL) {
		return;
	}

	/* now add anyone we find into the hashlist */

	/* skip past the headers */
	ptr = strstr(msg->cm_fields['M'], "\n\n");
	if (ptr != NULL) {
		ptr += 2;
	}
	else {
		ptr = strstr(msg->cm_fields['M'], "\n\r\n");
		if (ptr != NULL) {
			ptr += 3;
		}
	}

	/* the remaining lines are addresses */
	if (ptr != NULL) {
		ptr = strtok_r(ptr, "\n", &lasts);
		while (ptr != NULL) {
			char *pch = strdup(ptr);
			Put(mortuary, pch, strlen(pch), pch, NULL);
			ptr = strtok_r(NULL, "\n", &lasts);
		}
	}

	TRACE;
	CtdlFreeMessage(msg);
	TRACE;
}



/*
 * Fetch the "mortuary" - a list of dead buddies which we keep around forever
 * so we can remove them from any client's roster that still has them listed
 */
HashList *xmpp_fetch_mortuary(void) {
	HashList *mortuary = NewHash(1, NULL);
	if (!mortuary) {
		CtdlLogPrintf(CTDL_ALERT, "NewHash() failed!\n");
		return(NULL);
	}

        if (CtdlGetRoom(&CC->room, USERCONFIGROOM) != 0) {
		/* no config room exists - no further processing is required. */
                return(mortuary);
        }
        CtdlForEachMessage(MSGS_LAST, 1, NULL, XMPPMORTUARY, NULL,
                xmpp_fetch_mortuary_backend, (void *)mortuary );

	return(mortuary);
}



/*
 * Fetch the "mortuary" - a list of dead buddies which we keep around forever
 * so we can remove them from any client's roster that still has them listed
 */
void xmpp_store_mortuary(HashList *mortuary) {
	HashPos *HashPos;
	long len;
	void *Value;
	const char *Key;
	StrBuf *themsg;

	themsg = NewStrBuf();
	StrBufPrintf(themsg,	"Content-type: " XMPPMORTUARY "\n"
				"Content-transfer-encoding: 7bit\n"
				"\n"
	);

	HashPos = GetNewHashPos(mortuary, 0);
	while (GetNextHashPos(mortuary, HashPos, &len, &Key, &Value) != 0)
	{
		StrBufAppendPrintf(themsg, "%s\n", (char *)Value);
	}
	DeleteHashPos(&HashPos);

	/* Delete the old mortuary */
	CtdlDeleteMessages(USERCONFIGROOM, NULL, 0, XMPPMORTUARY);

	/* And save the new one to disk */
	quickie_message("Citadel", NULL, NULL, USERCONFIGROOM, ChrPtr(themsg), 4, "XMPP Mortuary");
	FreeStrBuf(&themsg);
}



/*
 * Upon logout we make an attempt to delete the whole roster, in order to
 * try to keep "ghost" buddies from remaining in the client-side roster.
 *
 * Since the client is probably not still alive, also remember the current
 * roster for next time so we can delete dead buddies then.
 */
void xmpp_massacre_roster(void)
{
	struct CitContext *cptr;
	int nContexts, i;
	int aide = (CC->user.axlevel >= AxAideU);
	HashList *mortuary = xmpp_fetch_mortuary();

	cptr = CtdlGetContextArray(&nContexts);
	if (cptr) {
		for (i=0; i<nContexts; i++) {
			if (cptr[i].logged_in) {
				if (
			   		(((cptr[i].cs_flags&CS_STEALTH)==0) || (aide))
			   		&& (cptr[i].user.usernum != CC->user.usernum)
			   	) {
					if (mortuary) {
						char *buddy = strdup(cptr[i].cs_inet_email);
						Put(mortuary, buddy, strlen(buddy), buddy, NULL);
					}
				}
			}
		}
		free (cptr);
	}

	if (mortuary) {
		xmpp_store_mortuary(mortuary);
		DeleteHash(&mortuary);
	}
}



/*
 * Stupidly, XMPP does not specify a way to tell the client to flush its client-side roster
 * and prepare to receive a new one.  So instead we remember every buddy we've ever told the
 * client about, and push delete operations out at the beginning of a session.
 */
void xmpp_delete_old_buddies_who_no_longer_exist_from_the_client_roster(void)
{
	long len;
	void *Value;
	const char *Key;
	HashList *mortuary = xmpp_fetch_mortuary();
	HashPos *HashPos = GetNewHashPos(mortuary, 0);

	/* FIXME delete from the list anyone who is currently online */

	while (GetNextHashPos(mortuary, HashPos, &len, &Key, &Value) != 0)
	{
		CtdlLogPrintf(CTDL_DEBUG, "\033[31mDELETE: %s\033[0m\n", (char *)Value);
		// FIXME xmpp_destroy_buddy((char *)Value);
	}
	DeleteHashPos(&HashPos);
	DeleteHash(&mortuary);
}
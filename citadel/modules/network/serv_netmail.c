/*
 * This module handles shared rooms, inter-Citadel mail, and outbound
 * mailing list processing.
 *
 * Copyright (c) 2000-2011 by the citadel.org team
 *
 *  This program is open source software; you can redistribute it and/or modify
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
 *
 * ** NOTE **   A word on the S_NETCONFIGS semaphore:
 * This is a fairly high-level type of critical section.  It ensures that no
 * two threads work on the netconfigs files at the same time.  Since we do
 * so many things inside these, here are the rules:
 *  1. begin_critical_section(S_NETCONFIGS) *before* begin_ any others.
 *  2. Do *not* perform any I/O with the client during these sections.
 *
 */

/*
 * Duration of time (in seconds) after which pending list subscribe/unsubscribe
 * requests that have not been confirmed will be deleted.
 */
#define EXP	259200	/* three days */

#include "sysdep.h"
#include <stdlib.h>
#include <unistd.h>
#include <stdio.h>
#include <fcntl.h>
#include <ctype.h>
#include <signal.h>
#include <pwd.h>
#include <errno.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <dirent.h>
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
#ifdef HAVE_SYSCALL_H
# include <syscall.h>
#else 
# if HAVE_SYS_SYSCALL_H
#  include <sys/syscall.h>
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
#include "user_ops.h"
#include "database.h"
#include "msgbase.h"
#include "internet_addressing.h"
#include "serv_network.h"
#include "clientsocket.h"
#include "file_ops.h"
#include "citadel_dirs.h"
#include "threads.h"

#ifndef HAVE_SNPRINTF
#include "snprintf.h"
#endif

#include "context.h"
#include "netconfig.h"
#include "netspool.h"
#include "ctdl_module.h"


/*
 * Deliver digest messages
 */
void network_deliver_digest(SpoolControl *sc) {
	char buf[SIZ];
	int i;
	struct CtdlMessage *msg = NULL;
	long msglen;
	char *recps = NULL;
	size_t recps_len = SIZ;
	struct recptypes *valid;
	namelist *nptr;
	char bounce_to[256];

	if (sc->num_msgs_spooled < 1) {
		fclose(sc->digestfp);
		sc->digestfp = NULL;
		return;
	}

	msg = malloc(sizeof(struct CtdlMessage));
	memset(msg, 0, sizeof(struct CtdlMessage));
	msg->cm_magic = CTDLMESSAGE_MAGIC;
	msg->cm_format_type = FMT_RFC822;
	msg->cm_anon_type = MES_NORMAL;

	sprintf(buf, "%ld", time(NULL));
	msg->cm_fields['T'] = strdup(buf);
	msg->cm_fields['A'] = strdup(CC->room.QRname);
	snprintf(buf, sizeof buf, "[%s]", CC->room.QRname);
	msg->cm_fields['U'] = strdup(buf);
	sprintf(buf, "room_%s@%s", CC->room.QRname, config.c_fqdn);
	for (i=0; buf[i]; ++i) {
		if (isspace(buf[i])) buf[i]='_';
		buf[i] = tolower(buf[i]);
	}
	msg->cm_fields['F'] = strdup(buf);
	msg->cm_fields['R'] = strdup(buf);

	/* Set the 'List-ID' header */
	msg->cm_fields['L'] = malloc(1024);
	snprintf(msg->cm_fields['L'], 1024,
		"%s <%ld.list-id.%s>",
		CC->room.QRname,
		CC->room.QRnumber,
		config.c_fqdn
	);

	/*
	 * Go fetch the contents of the digest
	 */
	fseek(sc->digestfp, 0L, SEEK_END);
	msglen = ftell(sc->digestfp);

	msg->cm_fields['M'] = malloc(msglen + 1);
	fseek(sc->digestfp, 0L, SEEK_SET);
	fread(msg->cm_fields['M'], (size_t)msglen, 1, sc->digestfp);
	msg->cm_fields['M'][msglen] = '\0';

	fclose(sc->digestfp);
	sc->digestfp = NULL;

	/* Now generate the delivery instructions */

	/* 
	 * Figure out how big a buffer we need to allocate
	 */
	for (nptr = sc->digestrecps; nptr != NULL; nptr = nptr->next) {
		recps_len = recps_len + strlen(nptr->name) + 2;
	}
	
	recps = malloc(recps_len);

	if (recps == NULL) {
		syslog(LOG_EMERG, "Cannot allocate %ld bytes for recps...\n", (long)recps_len);
		abort();
	}

	strcpy(recps, "");

	/* Each recipient */
	for (nptr = sc->digestrecps; nptr != NULL; nptr = nptr->next) {
		if (nptr != sc->digestrecps) {
			strcat(recps, ",");
		}
		strcat(recps, nptr->name);
	}

	/* Where do we want bounces and other noise to be heard?  Surely not the list members! */
	snprintf(bounce_to, sizeof bounce_to, "room_aide@%s", config.c_fqdn);

	/* Now submit the message */
	valid = validate_recipients(recps, NULL, 0);
	free(recps);
	if (valid != NULL) {
		valid->bounce_to = strdup(bounce_to);
		valid->envelope_from = strdup(bounce_to);
		CtdlSubmitMsg(msg, valid, NULL, 0);
	}
	CtdlFreeMessage(msg);
	free_recipients(valid);
}


/*
 * Deliver list messages to everyone on the list ... efficiently
 */
void network_deliver_list(struct CtdlMessage *msg, SpoolControl *sc) {
	char *recps = NULL;
	size_t recps_len = SIZ;
	struct recptypes *valid;
	namelist *nptr;
	char bounce_to[256];

	/* Don't do this if there were no recipients! */
	if (sc->listrecps == NULL) return;

	/* Now generate the delivery instructions */

	/* 
	 * Figure out how big a buffer we need to allocate
	 */
	for (nptr = sc->listrecps; nptr != NULL; nptr = nptr->next) {
		recps_len = recps_len + strlen(nptr->name) + 2;
	}
	
	recps = malloc(recps_len);

	if (recps == NULL) {
		syslog(LOG_EMERG, "Cannot allocate %ld bytes for recps...\n", (long)recps_len);
		abort();
	}

	strcpy(recps, "");

	/* Each recipient */
	for (nptr = sc->listrecps; nptr != NULL; nptr = nptr->next) {
		if (nptr != sc->listrecps) {
			strcat(recps, ",");
		}
		strcat(recps, nptr->name);
	}

	/* Where do we want bounces and other noise to be heard?  Surely not the list members! */
	snprintf(bounce_to, sizeof bounce_to, "room_aide@%s", config.c_fqdn);

	/* Now submit the message */
	valid = validate_recipients(recps, NULL, 0);
	free(recps);
	if (valid != NULL) {
		valid->bounce_to = strdup(bounce_to);
		valid->envelope_from = strdup(bounce_to);
		CtdlSubmitMsg(msg, valid, NULL, 0);
		free_recipients(valid);
	}
	/* Do not call CtdlFreeMessage(msg) here; the caller will free it. */
}




/*
 * Spools out one message from the list.
 */
void network_spool_msg(long msgnum, 
		       void *userdata, 
		       char *working_ignetcfg,
		       NetMap *the_netmap)
{
	SpoolControl *sc;
	int i;
	char *newpath = NULL;
	struct CtdlMessage *msg = NULL;
	namelist *nptr;
	maplist *mptr;
	struct ser_ret sermsg;
	FILE *fp;
	char filename[PATH_MAX];
	char buf[SIZ];
	int bang = 0;
	int send = 1;
	int delete_after_send = 0;	/* Set to 1 to delete after spooling */
	int ok_to_participate = 0;
	struct recptypes *valid;

	sc = (SpoolControl *)userdata;

	/*
	 * Process mailing list recipients
	 */
	if (sc->listrecps != NULL) {
		/* Fetch the message.  We're going to need to modify it
		 * in order to insert the [list name] in it, etc.
		 */
		msg = CtdlFetchMessage(msgnum, 1);
		if (msg != NULL) {
			int rlen;
			char *pCh;
			StrBuf *Subject, *FlatSubject;

			if (msg->cm_fields['V'] == NULL){
				/* local message, no enVelope */
				StrBuf *Buf;
				Buf = NewStrBuf();
				StrBufAppendBufPlain(Buf, msg->cm_fields['O'], -1, 0);
				StrBufAppendBufPlain(Buf, HKEY("@"), 0);
				StrBufAppendBufPlain(Buf, config.c_fqdn, -1, 0);
				
				msg->cm_fields['K'] = SmashStrBuf(&Buf);
			}
			else {
				msg->cm_fields['K'] = strdup (msg->cm_fields['V']);
			}
			/* Set the 'List-ID' header */
			if (msg->cm_fields['L'] != NULL) {
				free(msg->cm_fields['L']);
			}
			msg->cm_fields['L'] = malloc(1024);
			snprintf(msg->cm_fields['L'], 1024,
				"%s <%ld.list-id.%s>",
				CC->room.QRname,
				CC->room.QRnumber,
				config.c_fqdn
			);

			/* Prepend "[List name]" to the subject */
			if (msg->cm_fields['U'] == NULL) {
				Subject = NewStrBufPlain(HKEY("(no subject)"));
			}
			else {
				Subject = NewStrBufPlain(msg->cm_fields['U'], -1);
			}
			FlatSubject = NewStrBufPlain(NULL, StrLength(Subject));
			StrBuf_RFC822_to_Utf8(FlatSubject, Subject, NULL, NULL);

			rlen = strlen(CC->room.QRname);
			pCh  = strstr(ChrPtr(FlatSubject), CC->room.QRname);
			if ((pCh == NULL) ||
			    (*(pCh + rlen) != ']') ||
			    (pCh == ChrPtr(FlatSubject)) ||
			    (*(pCh - 1) != '[')
				)
			{
				StrBuf *tmp;
				StrBufPlain(Subject, HKEY("["));
				StrBufAppendBufPlain(Subject, CC->room.QRname, rlen, 0);
				StrBufAppendBufPlain(Subject, HKEY("] "), 0);
				StrBufAppendBuf(Subject, FlatSubject, 0);
				tmp = Subject;	Subject = FlatSubject;	FlatSubject = tmp; /* so we can free the right one... */
				StrBufRFC2047encode(&Subject, FlatSubject);
			}
			
			if (msg->cm_fields['U'] != NULL)
				free (msg->cm_fields['U']);
			msg->cm_fields['U'] = SmashStrBuf(&Subject);

			FreeStrBuf(&FlatSubject);

			/* else we won't modify the buffer, since the roomname is already here. */

			/* Set the recipient of the list message to the
			 * email address of the room itself.
			 * FIXME ... I want to be able to pick any address
			 */
			if (msg->cm_fields['R'] != NULL) {
				free(msg->cm_fields['R']);
			}
			msg->cm_fields['R'] = malloc(256);
			snprintf(msg->cm_fields['R'], 256,
				"room_%s@%s", CC->room.QRname,
				config.c_fqdn);
			for (i=0; msg->cm_fields['R'][i]; ++i) {
				if (isspace(msg->cm_fields['R'][i])) {
					msg->cm_fields['R'][i] = '_';
				}
			}

			/* Handle delivery */
			network_deliver_list(msg, sc);
			CtdlFreeMessage(msg);
		}
	}

	/*
	 * Process digest recipients
	 */
	if ((sc->digestrecps != NULL) && (sc->digestfp != NULL)) {
		msg = CtdlFetchMessage(msgnum, 1);
		if (msg != NULL) {
			fprintf(sc->digestfp,	" -----------------------------------"
						"------------------------------------"
						"-------\n");
			fprintf(sc->digestfp, "From: ");
			if (msg->cm_fields['A'] != NULL) {
				fprintf(sc->digestfp, "%s ", msg->cm_fields['A']);
			}
			if (msg->cm_fields['F'] != NULL) {
				fprintf(sc->digestfp, "<%s> ", msg->cm_fields['F']);
			}
			else if (msg->cm_fields['N'] != NULL) {
				fprintf(sc->digestfp, "@%s ", msg->cm_fields['N']);
			}
			fprintf(sc->digestfp, "\n");
			if (msg->cm_fields['U'] != NULL) {
				fprintf(sc->digestfp, "Subject: %s\n", msg->cm_fields['U']);
			}

			CC->redirect_buffer = NewStrBufPlain(NULL, SIZ);
			
			safestrncpy(CC->preferred_formats, "text/plain", sizeof CC->preferred_formats);
			CtdlOutputPreLoadedMsg(msg, MT_CITADEL, HEADERS_NONE, 0, 0, 0);

			StrBufTrim(CC->redirect_buffer);
			fwrite(HKEY("\n"), 1, sc->digestfp);
			fwrite(SKEY(CC->redirect_buffer), 1, sc->digestfp);
			fwrite(HKEY("\n"), 1, sc->digestfp);

			FreeStrBuf(&CC->redirect_buffer);

			sc->num_msgs_spooled += 1;
			free(msg);
		}
	}

	/*
	 * Process client-side list participations for this room
	 */
	if (sc->participates != NULL) {
		msg = CtdlFetchMessage(msgnum, 1);
		if (msg != NULL) {

			/* Only send messages which originated on our own Citadel
			 * network, otherwise we'll end up sending the remote
			 * mailing list's messages back to it, which is rude...
			 */
			ok_to_participate = 0;
			if (msg->cm_fields['N'] != NULL) {
				if (!strcasecmp(msg->cm_fields['N'], config.c_nodename)) {
					ok_to_participate = 1;
				}
				if (is_valid_node(NULL, 
						  NULL, 
						  msg->cm_fields['N'], 
						  working_ignetcfg, 
						  the_netmap) == 0)
				{
					ok_to_participate = 1;
				}
			}
			if (ok_to_participate) {
				if (msg->cm_fields['F'] != NULL) {
					free(msg->cm_fields['F']);
				}
				msg->cm_fields['F'] = malloc(SIZ);
				/* Replace the Internet email address of the actual
			 	* author with the email address of the room itself,
			 	* so the remote listserv doesn't reject us.
			 	* FIXME ... I want to be able to pick any address
			 	*/
				snprintf(msg->cm_fields['F'], SIZ,
					"room_%s@%s", CC->room.QRname,
					config.c_fqdn);
				for (i=0; msg->cm_fields['F'][i]; ++i) {
					if (isspace(msg->cm_fields['F'][i])) {
						msg->cm_fields['F'][i] = '_';
					}
				}

				/* 
				 * Figure out how big a buffer we need to allocate
			 	 */
				for (nptr = sc->participates; nptr != NULL; nptr = nptr->next) {

					if (msg->cm_fields['R'] == NULL) {
						free(msg->cm_fields['R']);
					}
					msg->cm_fields['R'] = strdup(nptr->name);
	
					valid = validate_recipients(nptr->name, NULL, 0);
					CtdlSubmitMsg(msg, valid, "", 0);
					free_recipients(valid);
				}
			
			}
			CtdlFreeMessage(msg);
		}
	}
	
	/*
	 * Process IGnet push shares
	 */
	msg = CtdlFetchMessage(msgnum, 1);
	if (msg != NULL) {
		size_t newpath_len;

		/* Prepend our node name to the Path field whenever
		 * sending a message to another IGnet node
		 */
		if (msg->cm_fields['P'] == NULL) {
			msg->cm_fields['P'] = strdup("username");
		}
		newpath_len = strlen(msg->cm_fields['P']) +
			 strlen(config.c_nodename) + 2;
		newpath = malloc(newpath_len);
		snprintf(newpath, newpath_len, "%s!%s",
			 config.c_nodename, msg->cm_fields['P']);
		free(msg->cm_fields['P']);
		msg->cm_fields['P'] = newpath;

		/*
		 * Determine if this message is set to be deleted
		 * after sending out on the network
		 */
		if (msg->cm_fields['S'] != NULL) {
			if (!strcasecmp(msg->cm_fields['S'], "CANCEL")) {
				delete_after_send = 1;
			}
		}

		/* Now send it to every node */
		if (sc->ignet_push_shares != NULL)
		  for (mptr = sc->ignet_push_shares; mptr != NULL;
		    mptr = mptr->next) {

			send = 1;

			/* Check for valid node name */
			if (is_valid_node(NULL, 
					  NULL, 
					  mptr->remote_nodename, 
					  working_ignetcfg, 
					  the_netmap) != 0)
			{
				syslog(LOG_ERR, "Invalid node <%s>\n", mptr->remote_nodename);
				send = 0;
			}

			/* Check for split horizon */
			syslog(LOG_DEBUG, "Path is %s\n", msg->cm_fields['P']);
			bang = num_tokens(msg->cm_fields['P'], '!');
			if (bang > 1) for (i=0; i<(bang-1); ++i) {
				extract_token(buf, msg->cm_fields['P'], i, '!', sizeof buf);
				syslog(LOG_DEBUG, "Compare <%s> to <%s>\n",
					buf, mptr->remote_nodename) ;
				if (!strcasecmp(buf, mptr->remote_nodename)) {
					send = 0;
					syslog(LOG_DEBUG, "Not sending to %s\n",
						mptr->remote_nodename);
				}
				else {
					syslog(LOG_DEBUG, "Sending to %s\n", mptr->remote_nodename);
				}
			}

			/* Send the message */
			if (send == 1) {

				/*
				 * Force the message to appear in the correct room
				 * on the far end by setting the C field correctly
				 */
				if (msg->cm_fields['C'] != NULL) {
					free(msg->cm_fields['C']);
				}
				if (!IsEmptyStr(mptr->remote_roomname)) {
					msg->cm_fields['C'] = strdup(mptr->remote_roomname);
				}
				else {
					msg->cm_fields['C'] = strdup(CC->room.QRname);
				}

				/* serialize it for transmission */
				serialize_message(&sermsg, msg);
				if (sermsg.len > 0) {

					/* write it to a spool file */
					snprintf(filename, sizeof filename,"%s/%s@%lx%x",
					 	ctdl_netout_dir,
					 	mptr->remote_nodename,
						time(NULL),
						rand()
					);
					syslog(LOG_DEBUG, "Appending to %s\n", filename);
					fp = fopen(filename, "ab");
					if (fp != NULL) {
						fwrite(sermsg.ser,
							sermsg.len, 1, fp);
						fclose(fp);
					}
					else {
						syslog(LOG_ERR, "%s: %s\n", filename, strerror(errno));
					}
	
					/* free the serialized version */
					free(sermsg.ser);
				}

			}
		}
		CtdlFreeMessage(msg);
	}

	/* update lastsent */
	sc->lastsent = msgnum;

	/* Delete this message if delete-after-send is set */
	if (delete_after_send) {
		CtdlDeleteMessages(CC->room.QRname, &msgnum, 1, "");
	}

}
/*
 * Support for blog rooms
 *
 * Copyright (c) 1999-2010 by the citadel.org team
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
#include "user_ops.h"
#include "database.h"
#include "msgbase.h"
#include "internet_addressing.h"
#include "serv_vcard.h"
#include "citadel_ldap.h"
#include "ctdl_module.h"

/*
 * sd sdhfksdjhkjsdfhk jsdhfkjsdfhkjsd hkfjhsdkjfhsdkjfhksdjfhsd
 */
int blog_upload_beforesave(struct CtdlMessage *msg) {
	char buf[SIZ];

	/* Only run this hook for blog rooms */
	if (CC->room.QRdefaultview != VIEW_BLOG) {
		return(0);
	}

	/* 
	 * If the message doesn't have an EUID, give it one.
	 */
	if (msg->cm_fields['E'] != NULL)
	{
		generate_uuid(buf);
		msg->cm_fields['E'] = strdup(buf);
	}

	/* Now allow the save to complete. */
	return(0);
}


CTDL_MODULE_INIT(blog)
{
	if (!threading)
	{
		CtdlRegisterMessageHook(blog_upload_beforesave, EVT_BEFORESAVE);
	}
	
	/* return our module id for the Log */
	return "blog";
}
#include "sysdep.h"
#include <ctype.h>
#include <errno.h>
#include <string.h>
#include <unistd.h>
#include <string.h>
#include <stdio.h>
#include <sys/select.h>
#include <fcntl.h>
#include <sys/types.h>
#include "libcitadel.h"

/**
 * @defgroup URLHandling ParsedURL object to handle connection data
 */

/**
 * @ingroup URLHandling
 * @brief frees a linked list of ParsedURL 
 * @param Url (list) of ParsedURL to be freet; Pointer is NULL'ed for the caller.
 */
void FreeURL(ParsedURL** Url)
{
	if (*Url != NULL) {
		FreeStrBuf(&(*Url)->URL);
		FreeStrBuf(&(*Url)->UrlWithoutCred);
		FreeStrBuf(&(*Url)->CurlCreds);
		if ((*Url)->Next != NULL)
			FreeURL(&(*Url)->Next);
		free(*Url);
		*Url = NULL;
	}
}

/**
 * @ingroup URLHandling
 * @brief parses the string provided with UrlStr into *Url
 * @param Url on success this contains the parsed object; needs to be free'd by caller.
 * @param UrlStr String we should parse into parts
 * @param DefaultPort Which is the default port here?
 */
int ParseURL(ParsedURL **Url, StrBuf *UrlStr, unsigned short DefaultPort)
{
	const char *pch, *pEndHost, *pPort, *pCredEnd, *pUserEnd;
	ParsedURL *url = (ParsedURL *)malloc(sizeof(ParsedURL));
	memset(url, 0, sizeof(ParsedURL));

	url->af = AF_INET;
	url->Port =  DefaultPort;
	/*
	 * http://username:passvoid@[ipv6]:port/url 
	 */
	url->URL = NewStrBufDup(UrlStr);
	url->Host = pch = ChrPtr(url->URL);
	url->LocalPart = strchr(pch, '/');
	if (url->LocalPart != NULL) {
		if ((*(url->LocalPart + 1) == '/') && 
		    (*(url->LocalPart - 1) == ':')) { /* TODO: find default port for this protocol... */
			url->Host = url->LocalPart + 2;
			url->LocalPart = strchr(url->Host, '/');
			if (url->LocalPart != NULL)
			{
			    StrBufPeek(url->URL, url->LocalPart, 0, '\0');
			    url->LocalPart++;
			}
		}
	}
	if (url->LocalPart == NULL) {
		url->LocalPart = pch + StrLength(url->URL);
	}

	pCredEnd = strchr(pch, '@');
	if (pCredEnd >= url->LocalPart)
		pCredEnd = NULL;
	if (pCredEnd != NULL)
	{
		url->User = url->Host;
		url->Host = pCredEnd + 1;
		pUserEnd = strchr(url->User, ':');
		
		if (pUserEnd > pCredEnd)
			pUserEnd = pCredEnd;
		else {
			url->Pass = pUserEnd + 1;
		}
		StrBufPeek(url->URL, pUserEnd, 0, '\0');
		StrBufPeek(url->URL, pCredEnd, 0, '\0');		
	}
	
	pPort = NULL;
	if (*url->Host == '[') {
		url->Host ++;
		pEndHost = strchr(url->Host, ']');
		if (pEndHost == NULL) {
			FreeStrBuf(&url->URL);
			free(url);
			return 0; /* invalid syntax, no ipv6 */
		}
		StrBufPeek(url->URL, pEndHost, 0, '\0');
		if (*(pEndHost + 1) == ':'){
			StrBufPeek(url->URL, pEndHost + 1, 0, '\0');
			pPort = pEndHost + 2;
		}
		url->af = AF_INET6;
	}
	else {
		pPort = strchr(url->Host, ':');
		if (pPort != NULL) {
			StrBufPeek(url->URL, pPort, 0, '\0');
			pPort ++;
		}
	}
	if (pPort != NULL)
		url->Port = atol(pPort);
	url->IsIP = inet_pton(url->af, url->Host, &url->Addr.sin6_addr);
	*Url = url;
	return 1;
}

void CurlPrepareURL(ParsedURL *Url)
{
	if (!strcmp(ChrPtr(Url->URL), "http"))
		Url->UrlWithoutCred = NewStrBufPlain(ChrPtr(Url->URL), -1);
	else 
		Url->UrlWithoutCred = NewStrBufPlain(HKEY("http://"));
	StrBufAppendBufPlain(Url->UrlWithoutCred, Url->Host, -1, 0);

	StrBufAppendBufPlain(Url->UrlWithoutCred, HKEY(":"), 0);

	StrBufAppendPrintf(Url->UrlWithoutCred, "%u", Url->Port);

	StrBufAppendBufPlain(Url->UrlWithoutCred, HKEY("/"), 0);
	if (Url->LocalPart)
	{
	    StrBufAppendBufPlain(Url->UrlWithoutCred, Url->LocalPart, -1, 0);
	}
	if (Url->User != NULL)
	{
		Url->CurlCreds = NewStrBufPlain(Url->User, -1);
		StrBufAppendBufPlain(Url->CurlCreds, HKEY(":"), 0);
		if (Url->Pass != NULL)
			StrBufAppendBufPlain(Url->CurlCreds, Url->Pass, -1, 0);
	}
	Url->PlainUrl = ChrPtr(Url->UrlWithoutCred);
}
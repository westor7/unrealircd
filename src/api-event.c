/************************************************************************
 *   IRC - Internet Relay Chat, api-event.c
 *   (C) 2001- Carsten Munk (Techie/Stskeeps) <stskeeps@tspre.org>
 *   and the UnrealIRCd team
 *
 *   See file AUTHORS in IRC package for additional names of
 *   the programmers. 
 *
 *   This program is free software; you can redistribute it and/or modify
 *   it under the terms of the GNU General Public License as published by
 *   the Free Software Foundation; either version 1, or (at your option)
 *   any later version.
 *
 *   This program is distributed in the hope that it will be useful,
 *   but WITHOUT ANY WARRANTY; without even the implied warranty of
 *   MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *   GNU General Public License for more details.
 *
 *   You should have received a copy of the GNU General Public License
 *   along with this program; if not, write to the Free Software
 *   Foundation, Inc., 675 Mass Ave, Cambridge, MA 02139, USA.
 */

#include "unrealircd.h"

ID_Copyright("(C) Carsten Munk 2001");

MODVAR Event *events = NULL;

extern EVENT(unrealdns_removeoldrecords);

/** Add an event, a function that will run at regular intervals.
 * @param module	Module that this event belongs to
 * @param name		Name of the event
 * @param event		The EVENT(function) to be called
 * @param data		The data to be passed to the function (or just NULL)
 * @param every_msec	Every <this> milliseconds the event will be called, but see notes.
 * @param count		After how many times we should stop calling this even (0 = infinite times)
 * @returns an Event struct
 * @notes UnrealIRCd will try to call the event every 'every_msec' milliseconds.
 *        However, in case of low traffic the minimum time is at least SOCKETLOOP_MAX_DELAY
 *        which is 250ms at the time of writing. Also, we reject any value below 100 msecs.
 *        The actual calling time will not be quicker than the specified every_msec but
 *        can be later, in case of high load, in very extreme cases even up to 1000 or 2000
 *        msec later but that would be very unusual. Just saying, it's not a guarantee..
 */
Event *EventAdd(Module *module, char *name, vFP event, void *data, long every_msec, int count)
{
	Event *newevent;
	if (!name || (every_msec < 0) || (count < 0) || !event)
	{
		if (module)
			module->errorcode = MODERR_INVALID;
		return NULL;
	}
	if (every_msec < 100)
	{
		ircd_log(LOG_ERROR, "[BUG] EventAdd() from module %s with suspiciously low every_msec value (%ld). "
		                    "Note that it is in milliseconds now (1000 = 1 second)!",
		                    module ? module->header->name : "???",
		                    every_msec);
		every_msec = 100;
	}
	newevent = safe_alloc(sizeof(Event));
	safe_strdup(newevent->name, name);
	newevent->count = count;
	newevent->every_msec = every_msec;
	newevent->event = event;
	newevent->data = data;
	newevent->last_run.tv_sec = timeofday_tv.tv_sec;
	newevent->last_run.tv_usec = timeofday_tv.tv_usec;
	newevent->owner = module;
	AddListItem(newevent,events);
	if (module)
	{
		ModuleObject *eventobj = safe_alloc(sizeof(ModuleObject));
		eventobj->object.event = newevent;
		eventobj->type = MOBJ_EVENT;
		AddListItem(eventobj, module->objects);
		module->errorcode = MODERR_NOERROR;
	}
	return newevent;
	
}

Event *EventMarkDel(Event *event)
{
	event->count = -1;
	return event;
}

Event *EventDel(Event *event)
{
	Event *p, *q;
	for (p = events; p; p = p->next)
	{
		if (p == event)
		{
			q = p->next;
			safe_free(p->name);
			DelListItem(p, events);
			if (p->owner)
			{
				ModuleObject *eventobjs;
				for (eventobjs = p->owner->objects; eventobjs; eventobjs = eventobjs->next)
				{
					if (eventobjs->type == MOBJ_EVENT && eventobjs->object.event == p)
					{
						DelListItem(eventobjs, p->owner->objects);
						safe_free(eventobjs);
						break;
					}
				}
			}
			safe_free(p);
			return q;		
		}
	}
	return NULL;
}

Event *EventFind(char *name)
{
	Event *eventptr;

	for (eventptr = events; eventptr; eventptr = eventptr->next)
		if (!strcmp(eventptr->name, name))
			return (eventptr);
	return NULL;
}

int EventMod(Event *event, EventInfo *mods)
{
	if (!event || !mods)
	{
		if (event && event->owner)
			event->owner->errorcode = MODERR_INVALID;
		return -1;
	}

	if (mods->flags & EMOD_EVERY)
		event->every_msec = mods->every_msec;
	if (mods->flags & EMOD_HOWMANY)
		event->count = mods->count;
	if (mods->flags & EMOD_NAME)
		safe_strdup(event->name, mods->name);
	if (mods->flags & EMOD_EVENT)
		event->event = mods->event;
	if (mods->flags & EMOD_DATA)
		event->data = mods->data;
	if (event->owner)
		event->owner->errorcode = MODERR_NOERROR;
	return 0;
}

void DoEvents(void)
{
	Event *eventptr;
	Event temp;

	for (eventptr = events; eventptr; eventptr = eventptr->next)
	{
		if (eventptr->count == -1)
			goto freeit;
		if ((eventptr->every_msec == 0) || minimum_msec_since_last_run(&eventptr->last_run, eventptr->every_msec))
		{
			(*eventptr->event)(eventptr->data);
			if (eventptr->count > 0)
			{
				eventptr->count--;
				if (eventptr->count == 0)
				{
freeit:
					temp.next = EventDel(eventptr);
					eventptr = &temp;
					continue;
				}
			}
		}
	}
}

void SetupEvents(void)
{
	/* Start events */
	EventAdd(NULL, "tunefile", save_tunefile, NULL, 300*1000, 0);
	EventAdd(NULL, "garbage", garbage_collect, NULL, GARBAGE_COLLECT_EVERY*1000, 0);
	EventAdd(NULL, "loop", loop_event, NULL, 1000, 0);
	EventAdd(NULL, "unrealdns_removeoldrecords", unrealdns_removeoldrecords, NULL, 15000, 0);
	EventAdd(NULL, "check_pings", check_pings, NULL, 1000, 0);
	EventAdd(NULL, "check_deadsockets", check_deadsockets, NULL, 1000, 0);
	EventAdd(NULL, "handshake_timeout", handshake_timeout, NULL, 1000, 0);
	EventAdd(NULL, "try_connections", try_connections, NULL, 2000, 0);
}

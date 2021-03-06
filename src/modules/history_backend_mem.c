/* src/modules/history_backend_mem.c - History Backend: memory
 * (C) Copyright 2019 Bram Matthys (Syzop) and the UnrealIRCd team
 * License: GPLv2
 */
#include "unrealircd.h"

/* This is the memory type backend. It is optimized for speed.
 * For example, per-channel, it caches the field "number of lines"
 * and "oldest record", so frequent cleaning operations such as
 * "delete any record older than time T" or "keep only N lines"
 * are executed as fast as possible.
 */

ModuleHeader MOD_HEADER
= {
	"history_backend_mem",
	"1.0",
	"History backend: memory",
	"UnrealIRCd Team",
	"unrealircd-5",
};

/* Defines */
#define OBJECTLEN	((NICKLEN > CHANNELLEN) ? NICKLEN : CHANNELLEN)
#define HISTORY_BACKEND_MEM_HASH_TABLE_SIZE 1019

/* Definitions (structs, etc.) */
typedef struct HistoryLogLine HistoryLogLine;
struct HistoryLogLine {
	HistoryLogLine *prev, *next;
	time_t t;
	MessageTag *mtags;
	char line[1];
};

typedef struct HistoryLogObject HistoryLogObject;
struct HistoryLogObject {
	HistoryLogObject *prev, *next;
	HistoryLogLine *head; /**< Start of the log (the earliest entry) */
	HistoryLogLine *tail; /**< End of the log (the latest entry) */
	int num_lines; /**< Number of lines of log */
	time_t oldest_t; /**< Oldest time in log */
	char name[OBJECTLEN+1];
};

/* Global variables */
static char siphashkey_history_backend_mem[SIPHASH_KEY_LENGTH];
HistoryLogObject *history_hash_table[HISTORY_BACKEND_MEM_HASH_TABLE_SIZE];

/* Forward declarations */
int hbm_history_add(char *object, MessageTag *mtags, char *line);
int hbm_history_del(char *object, int max_lines, long max_time);
int hbm_history_request(Client *client, char *object, HistoryFilter *filter);
int hbm_history_destroy(char *object);

MOD_INIT()
{
	HistoryBackendInfo hbi;

	MARK_AS_OFFICIAL_MODULE(modinfo);
	ModuleSetOptions(modinfo->handle, MOD_OPT_PERM, 1);

	memset(&history_hash_table, 0, sizeof(history_hash_table));
	siphash_generate_key(siphashkey_history_backend_mem);

	memset(&hbi, 0, sizeof(hbi));
	hbi.name = "mem";
	hbi.history_add = hbm_history_add;
	hbi.history_del = hbm_history_del;
	hbi.history_request = hbm_history_request;
	hbi.history_destroy = hbm_history_destroy;
	if (!HistoryBackendAdd(modinfo->handle, &hbi))
		return MOD_FAILED;

	return MOD_SUCCESS;
}

MOD_LOAD()
{
	return MOD_SUCCESS;
}

MOD_UNLOAD()
{
	return MOD_SUCCESS;
}

uint64_t hbm_hash(char *object)
{
	return siphash_nocase(object, siphashkey_history_backend_mem) % HISTORY_BACKEND_MEM_HASH_TABLE_SIZE;
}

HistoryLogObject *hbm_find_object(char *object)
{
	int hashv = hbm_hash(object);
	HistoryLogObject *h;

	for (h = history_hash_table[hashv]; h; h = h->next)
	{
		if (!strcasecmp(object, h->name))
			return h;
	}
	return NULL;
}

HistoryLogObject *hbm_find_or_add_object(char *object)
{
	int hashv = hbm_hash(object);
	HistoryLogObject *h;

	for (h = history_hash_table[hashv]; h; h = h->next)
	{
		if (!strcasecmp(object, h->name))
			return h;
	}
	/* Create new one */
	h = safe_alloc(sizeof(HistoryLogObject));
	strlcpy(h->name, object, sizeof(h->name));
	AddListItem(h, history_hash_table[hashv]);
	return h;
}

void hbm_delete_object_hlo(HistoryLogObject *h)
{
	int hashv = hbm_hash(h->name);

	DelListItem(h, history_hash_table[hashv]);
	safe_free(h);
}

void hbm_duplicate_mtags(HistoryLogLine *l, MessageTag *m)
{
	MessageTag *n;

	/* Duplicate all message tags */
	for (; m; m = m->next)
	{
		n = duplicate_mtag(m);
		AppendListItem(n, l->mtags);
	}
	n = find_mtag(l->mtags, "time");
	if (!n)
	{
		/* This is duplicate code from src/modules/server-time.c
		 * which seems silly.
		 */
		struct timeval t;
		struct tm *tm;
		time_t sec;
		char buf[64];

		gettimeofday(&t, NULL);
		sec = t.tv_sec;
		tm = gmtime(&sec);
		snprintf(buf, sizeof(buf), "%04d-%02d-%02dT%02d:%02d:%02d.%03dZ",
			tm->tm_year + 1900,
			tm->tm_mon + 1,
			tm->tm_mday,
			tm->tm_hour,
			tm->tm_min,
			tm->tm_sec,
			(int)(t.tv_usec / 1000));

		n = safe_alloc(sizeof(MessageTag));
		safe_strdup(n->name, "time");
		safe_strdup(n->value, buf);
		AddListItem(n, l->mtags);
	}
	/* Now convert the "time" message tag to something we can use in l->t */
	l->t = server_time_to_unix_time(n->value);
}

/** Add a line to a history object */
void hbm_history_add_line(HistoryLogObject *h, MessageTag *mtags, char *line)
{
	HistoryLogLine *l = safe_alloc(sizeof(HistoryLogLine) + strlen(line));
	strcpy(l->line, line); /* safe, see memory allocation above ^ */
	hbm_duplicate_mtags(l, mtags);
	if (h->tail)
	{
		/* append to tail */
		h->tail->next = l;
		l->prev = h->tail;
		h->tail = l;
	} else {
		/* no tail, no head */
		h->head = h->tail = l;
	}
	h->num_lines++;
	if ((l->t < h->oldest_t) || (h->oldest_t == 0))
		h->oldest_t = l->t;
}

/** Delete a line from a history object */
void hbm_history_del_line(HistoryLogObject *h, HistoryLogLine *l)
{
	if (l->prev)
		l->prev->next = l->next;
	if (l->next)
		l->next->prev = l->prev;
	if (h->head == l)
	{
		/* New head */
		h->head = l->next;
	}
	if (h->tail == l)
	{
		/* New tail */
		h->tail = l->prev; /* could be NULL now */
	}

	free_message_tags(l->mtags);
	safe_free(l);

	h->num_lines--;

	/* IMPORTANT: updating h->oldest_t takes place at the caller
	 * because it is in a better position to optimize the process
	 */
}

/** Add history entry */
int hbm_history_add(char *object, MessageTag *mtags, char *line)
{
	HistoryLogObject *h = hbm_find_or_add_object(object);
	hbm_history_add_line(h, mtags, line);
	return 0;
}

int can_receive_history(Client *client)
{
	if (HasCapability(client, "server-time"))
		return 1;
	return 0;
}

void hbm_send_line(Client *client, HistoryLogLine *l, char *batchid)
{
	if (can_receive_history(client))
	{
		if (BadPtr(batchid))
		{
			sendto_one(client, l->mtags, "%s", l->line);
		} else {
			MessageTag *m = safe_alloc(sizeof(MessageTag));
			m->name = "batch";
			m->value = batchid;
			AddListItem(m, l->mtags);
			sendto_one(client, l->mtags, "%s", l->line);
			DelListItem(m, l->mtags);
			safe_free(m);
		}
	} else {
		/* without server-time, log playback is a bit annoying, so skip it? */
	}
}

int hbm_history_request(Client *client, char *object, HistoryFilter *filter)
{
	HistoryLogObject *h = hbm_find_object(object);
	HistoryLogLine *l;
	char batch[BATCHLEN+1];

	if (!h || !can_receive_history(client))
		return 0;

	batch[0] = '\0';

	if (HasCapability(client, "batch"))
	{
		/* Start a new batch */
		generate_batch_id(batch);
		sendto_one(client, NULL, ":%s BATCH +%s chathistory %s", me.name, batch, object);
	}

	for (l = h->head; l; l = l->next)
		hbm_send_line(client, l, batch);

	/* End of batch */
	if (*batch)
		sendto_one(client, NULL, ":%s BATCH -%s", me.name, batch);
	return 1;
}

int hbm_history_del(char *object, int max_lines, long max_time)
{
	HistoryLogObject *h = hbm_find_object(object);
	HistoryLogLine *l, *l_next = NULL;
	long redline = TStime() - max_time;

	if (!h)
		return 0;

	/* First enforce 'max_time', after that enforce 'max_lines' */

	/* Checking for time */
	if (h->oldest_t < redline)
	{
		h->oldest_t = 0; /* recalculate in next loop */

		for (l = h->head; l; l = l_next)
		{
			l_next = l->next;
			if (l->t < redline)
			{
				hbm_history_del_line(h, l); /* too old, delete it */
				continue;
			}
			if ((h->oldest_t == 0) || (l->t < h->oldest_t))
				h->oldest_t = l->t;
		}
	}

	if (h->num_lines > max_lines)
	{
		h->oldest_t = 0; /* recalculate in next loop */

		for (l = h->head; l; l = l_next)
		{
			l_next = l->next;
			if (h->num_lines > max_lines)
			{
				hbm_history_del_line(h, l);
				continue;
			}
			if ((h->oldest_t == 0) || (l->t < h->oldest_t))
				h->oldest_t = l->t;
		}
	}

	return 1;
}

int hbm_history_destroy(char *object)
{
	HistoryLogObject *h = hbm_find_object(object);
	HistoryLogLine *l, *l_next;

	if (!h)
		return 0;

	for (l = h->head; l; l = l_next)
	{
		l_next = l->next;
		/* We could use hbm_history_del_line() here but
		 * it does unnecessary work, this is quicker.
		 * The only danger is that we may forget to free some
		 * fields that are added later there but not here.
		 */
		free_message_tags(l->mtags);
		safe_free(l);
	}

	hbm_delete_object_hlo(h);
	return 1;
}

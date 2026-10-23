#include "internal.h"

struct shared_event {
	uint8_t *wire;
	size_t size;
	size_t refs;
};
struct queued_event {
	shared_event *event;
	size_t offset;
	size_t start;
	size_t size;
	queued_event *next;
};
struct delivery_output {
	tg_output_id id;
	tg_output_options options;
	tg_delivery_info info;
	queued_event *head;
	queued_event *tail;
	size_t peeked;
	delivery_output *next;
};
struct tg_delivery {
	tg_limits limits;
	tg_overflow_policy policy;
	tg_delivery_info info;
	uint64_t sequence;
	uint64_t next_id;
	size_t count;
	delivery_output *outputs;
};
static delivery_output *output_find(const tg_delivery *d, tg_output_id id)
{
	for (delivery_output *o = d->outputs; o; o = o->next)
		if (o->id == id)
			return o;
	return nullptr;
}
static void record_remove(tg_delivery *d, delivery_output *o, bool drop)
{
	queued_event *q = o->head;
	o->head = q->next;
	if (!o->head)
		o->tail = nullptr;
	--o->info.queued_records;
	--d->info.queued_records;
	o->info.queued_bytes -= q->size;
	d->info.queued_bytes -= q->size;
	if (drop) {
		++o->info.records_dropped;
		++d->info.records_dropped;
	} else {
		++o->info.records_completed;
		++d->info.records_completed;
	}
	if (!--q->event->refs) {
		d->info.retained_bytes -= q->event->size;
		free(q->event->wire);
		free(q->event);
	}
	free(q);
}
tg_status tg_delivery_create(
	const tg_limits *limits, tg_overflow_policy policy, tg_delivery **out)
{
	/* Validate/copy bounds and create an empty fan-out engine. */
	if (!out)
		return TG_INVALID;
	*out = nullptr;
	tg_limits l;
	if (!tg_get_limits(limits, &l) || (policy != TG_OVERFLOW_DROP_NEWEST &&
						  policy != TG_OVERFLOW_FAIL))
		return TG_INVALID;
	tg_delivery *d = static_cast<tg_delivery *>(calloc(1, sizeof(*d)));
	if (!d)
		return TG_NO_MEMORY;
	d->limits = l;
	d->policy = policy;
	*out = d;
	return TG_OK;
}
tg_status tg_delivery_add_output(
	tg_delivery *d, const tg_output_options *options, tg_output_id *id)
{
	/* Validate limits and create a unique empty future-only queue. */
	if (!id)
		return TG_INVALID;
	*id = 0;
	if (!d || !options)
		return TG_INVALID;
	if (d->info.failed)
		return TG_BAD_STATE;
	if (options->kind != TG_OUTPUT_STREAM &&
		options->kind != TG_OUTPUT_DATAGRAM)
		return TG_INVALID;
	if (options->framing != TG_FRAMING_U32BE &&
		options->framing != TG_FRAMING_XML_DOCUMENT)
		return TG_INVALID;
	if (options->kind == TG_OUTPUT_DATAGRAM) {
		if (options->framing != TG_FRAMING_XML_DOCUMENT ||
			!options->max_datagram_bytes ||
			options->max_datagram_bytes > 65507)
			return TG_INVALID;
	} else if (options->max_datagram_bytes)
		return TG_INVALID;
	if (d->count == d->limits.max_outputs || d->next_id == UINT64_MAX)
		return TG_LIMIT;
	delivery_output *o =
		static_cast<delivery_output *>(calloc(1, sizeof(*o)));
	if (!o)
		return TG_NO_MEMORY;
	o->options = *options;
	if (!o->options.queue_bytes)
		o->options.queue_bytes = d->limits.queue_bytes;
	if (!o->options.queue_records)
		o->options.queue_records = d->limits.queue_records;
	o->id = ++d->next_id;
	o->next = d->outputs;
	d->outputs = o;
	++d->count;
	*id = o->id;
	return TG_OK;
}
tg_status tg_delivery_remove_output(tg_delivery *d, tg_output_id id)
{
	/*
	 * Count pending drops and release all of this output's
	 * references.
	 */
	if (!d || !id)
		return TG_INVALID;
	delivery_output **link = &d->outputs;
	while (*link && (*link)->id != id)
		link = &(*link)->next;
	if (!*link)
		return TG_INVALID;
	delivery_output *o = *link;
	*link = o->next;
	while (o->head)
		record_remove(d, o, true);
	free(o);
	--d->count;
	return TG_OK;
}
static bool queue_full(delivery_output *o, size_t size)
{
	size_t charge = size + (o->options.framing == TG_FRAMING_U32BE ? 4 : 0);
	return o->info.queued_records >= o->options.queue_records ||
	       charge > o->options.queue_bytes - o->info.queued_bytes ||
	       (o->options.kind == TG_OUTPUT_DATAGRAM &&
		       size > o->options.max_datagram_bytes);
}
tg_status tg_delivery_publish(
	tg_delivery *d, tg_bytes xml, tg_publish_report *report)
{
	/*
	 * Validate one complete event and preflight shared/global limits.
	 * Apply configured per-output loss/fatal policy; atomically retain one
	 * immutable payload and frame independent queues in admitted order.
	 * Preserve duplicates and existing partial-record offsets on failure.
	 */
	if (report)
		*report = {};
	if (!d || !report || !tg_span(xml))
		return TG_INVALID;
	if (d->info.failed)
		return TG_BAD_STATE;
	tg_xml_info info{};
	tg_status status = tg_xml(xml, &d->limits, &info);
	tg_xml_clear(&info);
	if (status != TG_OK)
		return status;
	if (xml.size > d->limits.retained_bytes - d->info.retained_bytes) {
		if (d->policy == TG_OVERFLOW_FAIL)
			d->info.failed = 1;
		return TG_LIMIT;
	}
	size_t accepted = 0, dropped = 0;
	for (delivery_output *o = d->outputs; o; o = o->next) {
		if (queue_full(o, xml.size))
			++dropped;
		else
			++accepted;
	}
	if (dropped && d->policy == TG_OVERFLOW_FAIL) {
		d->info.failed = 1;
		return TG_LIMIT;
	}
	shared_event *event = nullptr;
	queued_event *pending = nullptr;
	if (accepted) {
		event = static_cast<shared_event *>(calloc(1, sizeof(*event)));
		if (!event)
			return TG_NO_MEMORY;
		event->wire = static_cast<uint8_t *>(malloc(xml.size + 4));
		if (!event->wire) {
			free(event);
			return TG_NO_MEMORY;
		}
		event->size = xml.size;
		for (unsigned i = 0; i < 4; ++i)
			event->wire[i] = (uint8_t)(xml.size >> (24 - 8 * i));
		memcpy(event->wire + 4, xml.data, xml.size);
		for (size_t i = 0; i < accepted; ++i) {
			queued_event *q = static_cast<queued_event *>(
				calloc(1, sizeof(*q)));
			if (!q) {
				while (pending) {
					q = pending;
					pending = q->next;
					free(q);
				}
				free(event->wire);
				free(event);
				return TG_NO_MEMORY;
			}
			q->next = pending;
			pending = q;
		}
		d->info.retained_bytes += xml.size;
	}
	for (delivery_output *o = d->outputs; o; o = o->next) {
		if (queue_full(o, xml.size)) {
			++o->info.records_dropped;
			++d->info.records_dropped;
			if (o->options.kind == TG_OUTPUT_DATAGRAM &&
				xml.size > o->options.max_datagram_bytes) {
				++o->info.udp_oversize;
				++d->info.udp_oversize;
			}
			continue;
		}
		queued_event *q = pending;
		pending = pending->next;
		q->next = nullptr;
		q->event = event;
		++event->refs;
		q->start = o->options.framing == TG_FRAMING_U32BE ? 0 : 4;
		q->size = xml.size + 4 - q->start;
		if (o->tail)
			o->tail->next = q;
		else
			o->head = q;
		o->tail = q;
		++o->info.queued_records;
		++d->info.queued_records;
		o->info.queued_bytes += q->size;
		d->info.queued_bytes += q->size;
	}
	*report = {++d->sequence, accepted, dropped};
	return dropped || !d->count ? TG_DROPPED : TG_OK;
}
tg_status tg_delivery_peek(tg_delivery *d, tg_output_id id, tg_bytes *bytes)
{
	/*
	 * Borrow only the head record's remaining bytes, never
	 * interleave.
	 */
	if (!bytes)
		return TG_INVALID;
	*bytes = {};
	if (!d)
		return TG_INVALID;
	delivery_output *o = output_find(d, id);
	if (!o)
		return TG_INVALID;
	if (!o->head) {
		o->peeked = 0;
		return TG_MORE;
	}
	queued_event *q = o->head;
	*bytes = {q->event->wire + q->start + q->offset, q->size - q->offset};
	o->peeked = bytes->size;
	return TG_OK;
}
tg_status tg_delivery_consume(tg_delivery *d, tg_output_id id, size_t count)
{
	/*
	 * Validate last-peek bounds and datagram atomicity, advance the
	 * head offset, then release charges/references only on full completion.
	 */
	if (!d)
		return TG_INVALID;
	delivery_output *o = output_find(d, id);
	if (!o || count > o->peeked ||
		(o->options.kind == TG_OUTPUT_DATAGRAM && count &&
			count != o->peeked))
		return TG_INVALID;
	if (!count)
		return TG_OK;
	o->head->offset += count;
	o->peeked = 0;
	if (o->head->offset == o->head->size)
		record_remove(d, o, false);
	return TG_OK;
}
tg_status tg_delivery_snapshot(
	const tg_delivery *d, tg_output_id id, tg_delivery_info *info)
{
	/*
	 * Copy aggregate or per-output counters without exposing
	 * payloads.
	 */
	if (!info)
		return TG_INVALID;
	*info = {};
	if (!d)
		return TG_INVALID;
	if (!id) {
		*info = d->info;
		return TG_OK;
	}
	delivery_output *o = output_find(d, id);
	if (!o)
		return TG_INVALID;
	*info = o->info;
	info->failed = d->info.failed;
	return TG_OK;
}
void tg_delivery_destroy(tg_delivery *d)
{
	/*
	 * Discard queues, count remaining drops, and free shared storage.
	 */
	if (!d)
		return;
	while (d->outputs)
		tg_delivery_remove_output(d, d->outputs->id);
	free(d);
}

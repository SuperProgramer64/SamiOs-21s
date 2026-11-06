/* SPDX-License-Identifier: MIT
 * Keep storage bring-up and the newest statistics within a fixed pmsg budget.
 * Chunks are supplied in chronological order (head, then the unwrapped ring).
 */
#ifndef HAKIOS_LOGSELECT_H
#define HAKIOS_LOGSELECT_H
#include <stdint.h>
#include <stdbool.h>

struct hakios_storage_summary {
	char first[1024], last[512];
	char cores[256], faults[256];
	uint32_t cores_len, faults_len;
	uint32_t cores_detail_len;
	uint32_t first_len, last_len;
	bool first_full;
};

static bool
hakios_log_has(const volatile char *p, uint32_t n, const char *word)
{
	for (uint32_t i = 0; i < n; i++) {
		uint32_t j = 0;
		while (word[j] && i + j < n && p[i + j] == word[j]) j++;
		if (!word[j]) return true;
	}
	return false;
}

static void
hakios_log_save(char *dst, uint32_t *used, uint32_t capacity, const volatile char *p, uint32_t n)
{
	if (*used >= capacity - 1) return;
	if (n > capacity - 2 - *used) n = capacity - 2 - *used;
	for (uint32_t j = 0; j < n; j++) dst[(*used)++] = p[j];
	dst[(*used)++] = '\n';
	dst[*used] = 0;
}

static void
hakios_storage_summary_add(struct hakios_storage_summary *s, const volatile char *log, uint32_t len)
{
	for (uint32_t i = 0; i < len;) {
		uint32_t end = i;
		while (end < len && log[end] != '\n') {
			end++;
		}
		bool keep = false;
		/* A stamped kernel prefix is short. Do not mistake a later quoted string for it. */
		for (uint32_t j = i; j + 6 <= end && j - i < 48; j++) {
			if (log[j] == 'e' && log[j + 1] == 'm' && log[j + 2] == 'm' && log[j + 3] == 'c' &&
			    (log[j + 4] == '0' || log[j + 4] == '1') && log[j + 5] == ':') {
				keep = true;
				break;
			}
		}
		if (keep) {
			if (hakios_log_has(log + i, end - i, "failed") ||
			    hakios_log_has(log + i, end - i, "reset #")) {
				hakios_log_save(s->faults, &s->faults_len, sizeof(s->faults), log + i, end - i);
			}
			uint32_t n = end - i;
			if (n > 255) {
				n = 255;
			}
			if (!s->first_full && s->first_len + n + 1 <= sizeof(s->first)) {
				for (uint32_t k = 0; k < n; k++) {
					s->first[s->first_len++] = log[i + k];
				}
				s->first[s->first_len++] = '\n';
			} else {
				s->first_full = true;
				while (s->last_len + n + 1 > sizeof(s->last)) {
					uint32_t drop = 0;
					while (drop < s->last_len && s->last[drop++] != '\n') {
					}
					for (uint32_t k = drop; k < s->last_len; k++) {
						s->last[k - drop] = s->last[k];
					}
					s->last_len -= drop;
				}
				for (uint32_t k = 0; k < n; k++) {
					s->last[s->last_len++] = log[i + k];
				}
				s->last[s->last_len++] = '\n';
			}
		}
		if (hakios_log_has(log + i, end - i, "processors are running") ||
		    hakios_log_has(log + i, end - i, "has not joined") ||
		    hakios_log_has(log + i, end - i, "was not started") ||
		    hakios_log_has(log + i, end - i, "took an exception") ||
		    hakios_log_has(log + i, end - i, "low-entry=" ) ||
		    hakios_log_has(log + i, end - i, "no sign of it") ||
		    hakios_log_has(log + i, end - i, "joined the XNU scheduler")) {
			/* Always reserve 96 bytes for the final CPU count, even with eight
			 * verbose entry records. Preserve the earliest detail separately. */
			bool total = hakios_log_has(log + i, end - i, "processors are running");
			s->cores_len = s->cores_detail_len;
			hakios_log_save(s->cores, &s->cores_len,
			    total ? sizeof(s->cores) : sizeof(s->cores) - 96, log + i, end - i);
			if (!total) s->cores_detail_len = s->cores_len;
		}
		i = end + 1;
	}
}
#endif

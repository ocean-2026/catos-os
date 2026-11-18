#ifndef CLIP_H
#define CLIP_H

#include <stdint.h>

/*
 * clip.h -- v2.57 kernel-internal clipboard.
 *
 * A single linear UTF-8 buffer shared between the mouse cell selection
 * (producer) and the middle-click paste / `clip` builtin (consumers).
 * The buffer holds whole UTF-8 sequences only: the selection encoder works
 * per codepoint, and clip_set() additionally trims a dangling trailing
 * continuation byte so a consumer can never decode a half character.
 *
 * Capacity is deliberately small (4 KiB): this is a console selection
 * clipboard, not a file-transfer buffer.
 */

#define CLIP_CAP 4096

void  clip_set(const uint8_t *data, int len);
const char *clip_get(void);
int   clip_len(void);
int   clip_empty(void);
void  clip_clear(void);

#endif /* CLIP_H */

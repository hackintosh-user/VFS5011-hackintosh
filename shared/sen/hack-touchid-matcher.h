#ifndef __VFS5011_MATCHER_H
#define __VFS5011_MATCHER_H

#include "bozorth.h"

int vfs5011_extract_template(unsigned char *image_data, int width, int height,
                              struct xyt_struct *out_template);
int vfs5011_match_score(struct xyt_struct *probe, struct xyt_struct *enrolled);
int vfs5011_save_template(const char *path, struct xyt_struct *tmpl);
int vfs5011_load_template(const char *path, struct xyt_struct *tmpl);
int vfs5011_save_templates(const char *path, struct xyt_struct *tmpls, int count);
int vfs5011_load_templates(const char *path, struct xyt_struct *tmpls, int max_count, int *out_count);

#endif

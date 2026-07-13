/* The embedded block IDE (better-robotics/ide's ESP32 bundle, vendored into
 * web/ide/ by tools/sync-ide.sh). Each file is stored gzip-compressed —
 * ws_mqtt_bridge.c serves them with Content-Encoding: gzip and the browser
 * inflates; the chip never does. The table is GENERATED into src/ide_bundle.c
 * by tools/embed_ide.py (pre-build, same mechanism as dashboard_html.c). */
#pragma once

typedef struct {
    const char          *path;    /* request path, e.g. "/ide/app.js" */
    const char          *type;    /* Content-Type */
    const unsigned char *data;    /* body, lives in flash */
    unsigned int         len;
    unsigned char        gzipped; /* 0 = identity — audio: Chrome's media
                                   * pipeline rejects gzip on streamed audio
                                   * (ERR_CONTENT_DECODING_FAILED) */
} ide_file_t;

extern const ide_file_t ide_files[];
extern const unsigned int ide_files_count;

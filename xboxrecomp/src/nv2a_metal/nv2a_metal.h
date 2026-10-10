/* nv2a_metal.h -- Metal renderer for the pushbuffer executor (see nv2a_metal.m). */
#ifndef NV2A_METAL_H
#define NV2A_METAL_H

/* Register the Metal back end with the executor (nv2a_backend.h). Needs
 * macOS 14 / iOS 17 (dynamic vertex strides); check before calling. */
void nv2a_mtl_install(void);

/* Host hooks, weak defaults in nv2a_metal.m:
 *   nv2a_mtl_host_layer()  the CAMetalLayer to present to, made by the host
 *                          on its main thread (NULL: headless)
 *   nv2a_mtl_host_gate()   called before every GPU submission and drawable
 *                          request; blocks while GPU work is forbidden (iOS
 *                          in the background) */
void *nv2a_mtl_host_layer(void);
void nv2a_mtl_host_gate(void);

#endif

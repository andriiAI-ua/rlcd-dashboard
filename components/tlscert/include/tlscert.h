// HTTPS certificate of the settings page, from the own CA of the home network.
// NVS namespace "tls":
//   key     the private key of the installed certificate (PEM)
//   cert    the certificate, optionally with the CA chain after it (PEM)
//   newkey  a new key from tlscert_make_request(): it waits for its certificate
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

// The largest PEM text that the page can send (a key and a certificate chain).
#define TLSCERT_PEM_MAX 6000

// True when a key and a certificate are in NVS.
bool tlscert_installed(void);

// Read the certificate and the key for the HTTPS server. The caller frees both strings.
esp_err_t tlscert_load(char **cert, char **key);

// Make a new private key (ECDSA P-256) on the device and a certificate request (CSR) for host,
// for example "rlcd.lan". The key never leaves the device; it waits in NVS for its certificate.
esp_err_t tlscert_make_request(const char *host, char *csr, size_t len);

// Install a certificate from the page. pem: a certificate (for the key of the last request, or for
// the installed key), or a private key and a certificate made on a computer. The certificate must
// match the key and must not be expired. msg: the result or the error, for the user (Ukrainian).
esp_err_t tlscert_install(const char *host, const char *pem, char *msg, size_t len);

// Delete the key and the certificate: the page is HTTP only again.
esp_err_t tlscert_delete(void);

// A short text about the installed certificate, for the page.
void tlscert_describe(const char *host, char *buf, size_t len);

#ifdef __cplusplus
}
#endif

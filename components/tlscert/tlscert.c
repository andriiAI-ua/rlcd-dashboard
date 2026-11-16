// HTTPS certificate of the settings page (see tlscert.h).
#include "tlscert.h"
#include "texts.h"

#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include "esp_log.h"
#include "mbedtls/pk.h"
#include "mbedtls/x509_crt.h"
#include "mbedtls/x509_csr.h"
#include "nvs.h"
#include "psa/crypto.h"

static const char *TAG = "tlscert";
static const char *NAMESPACE = "tls";

#define KEY_PEM_MAX 512

/* NVS */

// Read a NUL-terminated blob. The caller frees *out.
static esp_err_t read_blob(const char *name, char **out)
{
    *out = NULL;
    nvs_handle_t h;
    esp_err_t err = nvs_open(NAMESPACE, NVS_READONLY, &h);
    if (err != ESP_OK) {
        return err;
    }
    size_t len = 0;
    err = nvs_get_blob(h, name, NULL, &len);
    if (err == ESP_OK && len > 1) {
        *out = malloc(len);
        err = *out ? nvs_get_blob(h, name, *out, &len) : ESP_ERR_NO_MEM;
        if (err == ESP_OK) {
            (*out)[len - 1] = '\0';
        } else {
            free(*out);
            *out = NULL;
        }
    } else if (err == ESP_OK) {
        err = ESP_ERR_NOT_FOUND;
    }
    nvs_close(h);
    return err;
}

static bool has_blob(const char *name)
{
    nvs_handle_t h;
    if (nvs_open(NAMESPACE, NVS_READONLY, &h) != ESP_OK) {
        return false;
    }
    size_t len = 0;
    const bool found = nvs_get_blob(h, name, NULL, &len) == ESP_OK && len > 1;
    nvs_close(h);
    return found;
}

// Write the key and the certificate (either can be NULL), and erase one entry (can be NULL).
static esp_err_t write_blobs(const char *key, const char *cert, const char *newkey, const char *erase)
{
    nvs_handle_t h;
    esp_err_t err = nvs_open(NAMESPACE, NVS_READWRITE, &h);
    if (err != ESP_OK) {
        return err;
    }
    if (key) {
        err = nvs_set_blob(h, "key", key, strlen(key) + 1);
    }
    if (err == ESP_OK && cert) {
        err = nvs_set_blob(h, "cert", cert, strlen(cert) + 1);
    }
    if (err == ESP_OK && newkey) {
        err = nvs_set_blob(h, "newkey", newkey, strlen(newkey) + 1);
    }
    if (err == ESP_OK && erase) {
        const esp_err_t e = nvs_erase_key(h, erase);
        err = e == ESP_ERR_NVS_NOT_FOUND ? ESP_OK : e;
    }
    if (err == ESP_OK) {
        err = nvs_commit(h);
    }
    nvs_close(h);
    return err;
}

bool tlscert_installed(void)
{
    return has_blob("key") && has_blob("cert");
}

esp_err_t tlscert_load(char **cert, char **key)
{
    *cert = NULL;
    *key = NULL;
    esp_err_t err = read_blob("cert", cert);
    if (err == ESP_OK) {
        err = read_blob("key", key);
    }
    if (err != ESP_OK) {
        free(*cert);
        free(*key);
        *cert = NULL;
        *key = NULL;
    }
    return err;
}

esp_err_t tlscert_delete(void)
{
    nvs_handle_t h;
    esp_err_t err = nvs_open(NAMESPACE, NVS_READWRITE, &h);
    if (err == ESP_OK) {
        nvs_erase_all(h);
        err = nvs_commit(h);
        nvs_close(h);
    }
    ESP_LOGI(TAG, "The HTTPS key and certificate are deleted");
    return err;
}

/* Request */

esp_err_t tlscert_make_request(const char *host, char *csr, size_t len)
{
    psa_crypto_init();      // the system starts PSA; a second call does nothing
    psa_key_attributes_t attr = PSA_KEY_ATTRIBUTES_INIT;
    psa_set_key_type(&attr, PSA_KEY_TYPE_ECC_KEY_PAIR(PSA_ECC_FAMILY_SECP_R1));
    psa_set_key_bits(&attr, 256);
    psa_set_key_usage_flags(&attr, PSA_KEY_USAGE_SIGN_HASH | PSA_KEY_USAGE_SIGN_MESSAGE | PSA_KEY_USAGE_EXPORT);
    psa_set_key_algorithm(&attr, PSA_ALG_ECDSA(PSA_ALG_SHA_256));
    mbedtls_svc_key_id_t id = MBEDTLS_SVC_KEY_ID_INIT;
    if (psa_generate_key(&attr, &id) != PSA_SUCCESS) {
        ESP_LOGE(TAG, "Cannot make a key");
        return ESP_FAIL;
    }
    mbedtls_pk_context pk;
    mbedtls_pk_init(&pk);
    int r = mbedtls_pk_copy_from_psa(id, &pk);
    psa_destroy_key(id);

    unsigned char *key_pem = calloc(1, KEY_PEM_MAX);
    if (!key_pem) {
        mbedtls_pk_free(&pk);
        return ESP_ERR_NO_MEM;
    }
    if (r == 0) {
        r = mbedtls_pk_write_key_pem(&pk, key_pem, KEY_PEM_MAX);
    }

    mbedtls_x509write_csr w;
    mbedtls_x509write_csr_init(&w);
    mbedtls_x509write_csr_set_md_alg(&w, MBEDTLS_MD_SHA256);
    mbedtls_x509write_csr_set_key(&w, &pk);
    char subject[96];
    snprintf(subject, sizeof(subject), "CN=%s", host);
    if (r == 0) {
        r = mbedtls_x509write_csr_set_subject_name(&w, subject);
    }
    // The browsers check the name in the Subject Alternative Name, not in CN.
    mbedtls_x509_san_list san;
    memset(&san, 0, sizeof(san));
    san.node.type = MBEDTLS_X509_SAN_DNS_NAME;
    san.node.san.unstructured_name.p = (unsigned char *)host;
    san.node.san.unstructured_name.len = strlen(host);
    if (r == 0) {
        r = mbedtls_x509write_csr_set_subject_alternative_name(&w, &san);
    }
    if (r == 0) {
        r = mbedtls_x509write_csr_pem(&w, (unsigned char *)csr, len);
    }
    mbedtls_x509write_csr_free(&w);
    mbedtls_pk_free(&pk);

    esp_err_t err = ESP_FAIL;
    if (r == 0) {
        err = write_blobs(NULL, NULL, (const char *)key_pem, NULL);
    } else {
        ESP_LOGE(TAG, "Cannot make the request: -0x%04x", (unsigned)-r);
    }
    memset(key_pem, 0, KEY_PEM_MAX);
    free(key_pem);
    if (err == ESP_OK) {
        ESP_LOGI(TAG, "A new key and a request for %s are ready", host);
    }
    return err;
}

/* Install */

// Copy the PEM blocks of a text: the first private key to *key, the certificates to *certs in their order.
// The caller frees both.
static esp_err_t split_pem(const char *pem, char **key, char **certs)
{
    const size_t cap = 2 * strlen(pem) + 16;
    *key = calloc(1, cap);
    *certs = calloc(1, cap);
    if (!*key || !*certs) {
        free(*key);
        free(*certs);
        return ESP_ERR_NO_MEM;
    }
    const char *p = pem;
    while ((p = strstr(p, "-----BEGIN ")) != NULL) {
        const char *label = p + 11;
        const char *label_end = strstr(label, "-----");
        const char *end = label_end ? strstr(label_end + 5, "-----END ") : NULL;
        const char *end_dashes = end ? strstr(end + 9, "-----") : NULL;
        if (!end_dashes) {
            break;
        }
        end_dashes += 5;
        const size_t label_len = (size_t)(label_end - label);
        char *dst = NULL;
        if (label_len == 11 && strncmp(label, "CERTIFICATE", 11) == 0) {
            dst = *certs;
        } else if (label_len >= 11 && strncmp(label_end - 11, "PRIVATE KEY", 11) == 0 && (*key)[0] == '\0') {
            dst = *key;
        }
        if (dst) {
            strncat(dst, p, (size_t)(end_dashes - p));
            strcat(dst, "\n");
        }
        p = end_dashes;
    }
    return ESP_OK;
}

// Return true if the certificate has the name as a DNS name in Subject Alternative Name.
static bool has_name(const mbedtls_x509_crt *crt, const char *host)
{
    const size_t n = strlen(host);
    for (const mbedtls_x509_sequence *cur = &crt->subject_alt_names; cur && cur->buf.p; cur = cur->next) {
        if ((cur->buf.tag & MBEDTLS_ASN1_TAG_VALUE_MASK) == MBEDTLS_X509_SAN_DNS_NAME && cur->buf.len == n &&
            strncasecmp((const char *)cur->buf.p, host, n) == 0) {
            return true;
        }
    }
    return false;
}

// Parse a key and check that the certificate is for it. Return 0 when they match.
static int check_pair(const mbedtls_x509_crt *crt, const char *key_pem)
{
    mbedtls_pk_context pk;
    mbedtls_pk_init(&pk);
    int r = mbedtls_pk_parse_key(&pk, (const unsigned char *)key_pem, strlen(key_pem) + 1, NULL, 0);
    if (r == 0) {
        r = mbedtls_pk_check_pair(&crt->pk, &pk);
    }
    mbedtls_pk_free(&pk);
    return r;
}

static void say(char *msg, size_t len, const char *fmt, ...) __attribute__((format(printf, 3, 4)));
static void say(char *msg, size_t len, const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(msg, len, fmt, ap);
    va_end(ap);
}

esp_err_t tlscert_install(const char *host, const char *pem, char *msg, size_t len)
{
    char *key = NULL, *certs = NULL;
    if (split_pem(pem, &key, &certs) != ESP_OK) {
        say(msg, len, tr(T_MSG_NO_MEMORY));
        return ESP_ERR_NO_MEM;
    }
    esp_err_t err = ESP_FAIL;
    mbedtls_x509_crt crt;
    mbedtls_x509_crt_init(&crt);
    char *stored = NULL;
    const char *use_key = NULL;
    const char *erase = NULL;

    if (certs[0] == '\0') {
        say(msg, len, tr(T_CERT_NO_CERT));
        goto done;
    }
    const int parsed = mbedtls_x509_crt_parse(&crt, (const unsigned char *)certs, strlen(certs) + 1);
    if (parsed < 0) {
        say(msg, len, tr(T_CERT_UNREADABLE), (unsigned)-parsed);
        goto done;
    }
    if (key[0]) {
        // A key and a certificate from a computer.
        if (check_pair(&crt, key) != 0) {
            say(msg, len, tr(T_CERT_NOT_PAIR));
            goto done;
        }
        use_key = key;
        erase = "newkey";
    } else {
        // A certificate for the key of the last request; else for the installed key (a renewal).
        if (read_blob("newkey", &stored) == ESP_OK && check_pair(&crt, stored) == 0) {
            use_key = stored;
            erase = "newkey";
        } else {
            free(stored);
            stored = NULL;
            if (read_blob("key", &stored) == ESP_OK && check_pair(&crt, stored) == 0) {
                use_key = stored;
            }
        }
        if (!use_key) {
            say(msg, len, tr(T_CERT_OTHER_KEY));
            goto done;
        }
    }
    if (mbedtls_x509_time_is_past(&crt.valid_to)) {
        say(msg, len, tr(T_CERT_EXPIRED));
        goto done;
    }
    if (write_blobs(use_key, certs, NULL, erase) != ESP_OK) {
        say(msg, len, tr(T_CERT_WRITE_FAILED));
        goto done;
    }
    say(msg, len, tr(T_CERT_INSTALLED), crt.valid_to.day, crt.valid_to.mon, crt.valid_to.year,
        has_name(&crt, host) ? "" : tr(T_CERT_NO_NAME), host);
    ESP_LOGI(TAG, "A certificate is installed");
    err = ESP_OK;

done:
    mbedtls_x509_crt_free(&crt);
    if (stored) {
        memset(stored, 0, strlen(stored));
        free(stored);
    }
    memset(key, 0, strlen(key));
    free(key);
    free(certs);
    return err;
}

void tlscert_describe(const char *host, char *buf, size_t len)
{
    const char *waiting = has_blob("newkey") ? tr(T_CERT_NEW_KEY_WAITS) : "";
    char *cert = NULL;
    if (read_blob("cert", &cert) != ESP_OK) {
        snprintf(buf, len, tr(T_CERT_NONE), waiting);
        return;
    }
    mbedtls_x509_crt crt;
    mbedtls_x509_crt_init(&crt);
    if (mbedtls_x509_crt_parse(&crt, (const unsigned char *)cert, strlen(cert) + 1) < 0) {
        snprintf(buf, len, tr(T_CERT_UNREADABLE_NOW), waiting);
    } else {
        char issuer[64];
        if (mbedtls_x509_dn_gets(issuer, sizeof(issuer), &crt.issuer) < 0) {
            strcpy(issuer, "?");
        }
        snprintf(buf, len, tr(T_CERT_ABOUT), has_name(&crt, host) ? host : tr(T_CERT_OTHER_NAME),
                 issuer, crt.valid_to.day, crt.valid_to.mon, crt.valid_to.year,
                 mbedtls_x509_time_is_past(&crt.valid_to) ? tr(T_CERT_EXPIRED_MARK) : "", waiting);
    }
    mbedtls_x509_crt_free(&crt);
    free(cert);
}

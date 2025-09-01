#include <stddef.h>
#include "doordvr_export.h"
#include "testmain.h"
#include "intelligentdriverassistant.h"
#include "AdasDetect.h"
#include "IDAFrameDraw.h"
#include "rtmp_playback.h"
#include "server.h"
#include "bubiao1078apidemo.h"
#include <ifaddrs.h>
#include <arpa/inet.h>
#include <net/if.h>
#include "pcmPlayer.h"
#include "ms_netdvr_4gapi.h"
#include <sys/random.h>
#include "param.h"
#include <math.h> 

#ifdef __cplusplus
extern "C"
{
#endif
#ifndef RK_UUID_STRLEN
#define RK_UUID_STRLEN 36
#endif
int rk_fill_random(uint8_t *buf, size_t len) {
    if (!buf || !len) return -1;

    size_t off = 0;
    while (off < len) {
        ssize_t n = getrandom(buf + off, len - off, 0);
        if (n > 0) { off += (size_t)n; continue; }
        if (n < 0 && errno == EINTR) continue;
        break; 
    }
    if (off == len) return 0;

    int fd = open("/dev/urandom", O_RDONLY);
    if (fd < 0) return -1;
    off = 0;
    while (off < len) {
        ssize_t n = read(fd, buf + off, len - off);
        if (n > 0) { off += (size_t)n; continue; }
        if (n < 0 && errno == EINTR) continue;
        close(fd);
        return -1;
    }
    close(fd);
    return 0;
}

int rk_generate_uuid_v4(char *out, size_t out_len) {
    if (!out || out_len < RK_UUID_STRLEN + 1) return -1;
    uint8_t r[16];
    if (rk_fill_random(r, sizeof r) != 0) return -1;

    r[6] = (uint8_t)((r[6] & 0x0F) | 0x40);  /* version=4 */
    r[8] = (uint8_t)((r[8] & 0x3F) | 0x80);  /* variant=RFC4122 */

    int written = snprintf(out, out_len,
        "%02x%02x%02x%02x-"
        "%02x%02x-"
        "%02x%02x-"
        "%02x%02x-"
        "%02x%02x%02x%02x%02x%02x",
        r[0], r[1], r[2], r[3],
        r[4], r[5],
        r[6], r[7],
        r[8], r[9],
        r[10], r[11], r[12], r[13], r[14], r[15]);
    return (written == RK_UUID_STRLEN) ? 0 : -1;
}

void rk_norm_copy(const char *in, char *out, size_t out_len) {
    if (!out || !out_len) return;
    out[0] = '\0';
    if (!in) return;
    const char *s = in; while (*s && isspace((unsigned char)*s)) s++;
    const char *e = in + strlen(in); while (e > s && isspace((unsigned char)e[-1])) e--;
    size_t n = (size_t)(e - s);
    if (n >= out_len) n = out_len - 1;
    for (size_t i = 0; i < n; ++i) out[i] = (char)tolower((unsigned char)s[i]);
    out[n] = '\0';
}
void rk_norm_mac(const char *in, char *out, size_t out_len) {
    if (!out || !out_len) return;
    size_t w = 0; out[0] = '\0';
    if (!in) return;
    for (const char *p = in; *p && w + 1 < out_len; ++p)
        if (isxdigit((unsigned char)*p))
            out[w++] = (char)tolower((unsigned char)*p);
    out[w] = '\0';
}

uint64_t rk_fnv1a64(const void *data, size_t len) {
    const uint8_t *p = (const uint8_t*)data;
    uint64_t h = 1469598103934665603ULL;             
    while (len--) { h ^= *p++; h *= 1099511628211ULL; }
    return h;
}

void rk_build_fingerprint_from_request(const CreateKitRequest_t *request,
                                              char *fp_out, size_t fp_len)
{
    char imei[128]={0}, mac[128]={0}, sn[128]={0}, vin[128]={0};
    rk_norm_copy(request->kit.imei,         imei, sizeof(imei));
    rk_norm_mac (request->kit.mac,          mac,  sizeof(mac));
    rk_norm_copy(request->kit.serialNumber, sn,   sizeof(sn));
    rk_norm_copy(request->kit.vinNumber,    vin,  sizeof(vin));
    snprintf(fp_out, fp_len, "imei=%s|mac=%s|sn=%s|vin=%s", imei, mac, sn, vin);
}

int rk_pick_dbdir(char *out, size_t out_len) {
    const char *cands[] = { "/var/lib/rk-kitdb", "./rk-kitdb" };
    for (size_t i = 0; i < sizeof(cands)/sizeof(cands[0]); ++i) {
        if (mkdir(cands[i], 0755) == 0 || errno == EEXIST) {
            snprintf(out, out_len, "%s", cands[i]);
            return 0;
        }
    }
    return -1;
}

int rk_get_or_create_kitid(const char *fingerprint,
                                  char *kitid_out, size_t kitid_len)
{
    if (!fingerprint || !kitid_out || kitid_len < RK_UUID_STRLEN + 1) return -1;

    char dbdir[256];
    if (rk_pick_dbdir(dbdir, sizeof(dbdir)) != 0) return -2;

    uint64_t key = rk_fnv1a64(fingerprint, strlen(fingerprint));

    char path[512];
    snprintf(path, sizeof(path), "%s/%016llx.kitid",
             dbdir, (unsigned long long)key);

    int fd = open(path, O_RDONLY);
    if (fd >= 0) {
        char buf[64]={0};
        ssize_t n = read(fd, buf, RK_UUID_STRLEN);
        close(fd);
        if (n == RK_UUID_STRLEN) {
            memcpy(kitid_out, buf, RK_UUID_STRLEN);
            kitid_out[RK_UUID_STRLEN] = '\0';
            return 0;
        }
    }

    fd = open(path, O_WRONLY | O_CREAT | O_EXCL, 0644);
    if (fd < 0) {
        if (errno == EEXIST) {
            int fd2 = open(path, O_RDONLY);
            if (fd2 >= 0) {
                char buf[64]={0};
                ssize_t n2 = read(fd2, buf, RK_UUID_STRLEN);
                close(fd2);
                if (n2 == RK_UUID_STRLEN) {
                    memcpy(kitid_out, buf, RK_UUID_STRLEN);
                    kitid_out[RK_UUID_STRLEN] = '\0';
                    return 0;
                }
            }
        }
        return -3;
    }

    char new_id[RK_UUID_STRLEN + 1];
    if (rk_generate_uuid_v4(new_id, sizeof new_id) != 0) {
        close(fd); unlink(path); return -4;
    }
    if (write(fd, new_id, RK_UUID_STRLEN) != RK_UUID_STRLEN ||
        write(fd, "\n", 1) != 1) { close(fd); unlink(path); return -5; }
    fsync(fd);
    close(fd);

    strncpy(kitid_out, new_id, kitid_len - 1);
    kitid_out[kitid_len - 1] = '\0';
    return 0;
}





static int gps_is_valid_basic(const SSYFI_GPS* g) {
    if (!g) return 0;
    if (g->ExistStatus == 0) return 0;       // 无有效数据帧
    if (g->cGpsStatus <= 0) return 0;        // 未定位（按你们的枚举，>0 有效）
    unsigned char lat_deg = (unsigned char)g->cLatitudeDegree;
    unsigned char lat_min = (unsigned char)g->cLatitudeCent;
    unsigned char lon_deg = (unsigned char)g->cLongitudeDegree;
    unsigned char lon_min = (unsigned char)g->cLongitudeCent;
    if (lat_deg > 90 || lon_deg > 180) return 0;
    if (lat_min >= 60 || lon_min >= 60) return 0;
    return 1;
}

static int dir_sign(char dir) {               // 'N','E'→+1；'S','W'→-1
    return (dir == 'S' || dir == 'W') ? -1 : +1;
}

static double dms_to_deg(unsigned char deg, unsigned char min, long frac_min_1e5, int sign_nswe) {
    double minutes = (double)min + (frac_min_1e5 / 100000.0); // 0.mmmmm 分
    double d = (double)deg + minutes / 60.0;
    return (sign_nswe >= 0) ? d : -d;
}

static double scaled_to_deg(long scaled, char frac_len, int sign_nswe) {
    double d = (double)scaled;
    for (int i = 0; i < (int)frac_len; ++i) d /= 10.0;
    return (sign_nswe >= 0) ? d : -d;
}


static int estimate_radius_r95_from_hdop(unsigned char hdop_int, unsigned char hdop_frac,
                                         unsigned char sats, int cGpsStatus)
{
    double hdop = (double)hdop_int + ((double)hdop_frac)/100.0;
    double UERE = 5.0;                      
    if (cGpsStatus >= 3) { /* 3D fix */ UERE = 5.0; }

    double sigma = (hdop > 0.0 ? hdop : 6.0) * UERE;  
    int r95 = (int)ceil(2.45 * sigma);            

    if (hdop <= 0.0) { 
        if (sats >= 8)      r95 = 20;
        else if (sats >= 6) r95 = 30;
        else if (sats >= 4) r95 = 60;
        else                r95 = 120;
    }

    if (r95 < 3) r95 = 3;
    if (r95 > 1000) r95 = 1000;
    return r95;
}

int parse_gps_to_deg(SSYFI_GPS* g, double* out_lat, double* out_lon, int* out_radius_m,
                            char* out_err_type, char* out_err_desc, size_t err_desc_sz)
{
    if (out_err_type) out_err_type[0] = '\0';
    if (out_err_desc && err_desc_sz) out_err_desc[0] = '\0';

 if (!g) {
    if (out_err_type) strcpy(out_err_type, "GPS_INVALID");
    if (out_err_desc && err_desc_sz) snprintf(out_err_desc, err_desc_sz, "g is NULL");
    return 0;
}
if (g->ExistStatus == 0) {
    if (out_err_type) strcpy(out_err_type, "GPS_NO_FRAME");
    if (out_err_desc && err_desc_sz) snprintf(out_err_desc, err_desc_sz, "ExistStatus=0 (no valid frame)");
    return 0;
}
if (g->cGpsStatus <= 0) {
    if (out_err_type) strcpy(out_err_type, "GPS_NO_FIX");
    if (out_err_desc && err_desc_sz) snprintf(out_err_desc, err_desc_sz, "cGpsStatus=%d (no fix)", g->cGpsStatus);
    return 0;
}
unsigned char lat_deg = (unsigned char)g->cLatitudeDegree;
unsigned char lat_min = (unsigned char)g->cLatitudeCent;
unsigned char lon_deg = (unsigned char)g->cLongitudeDegree;
unsigned char lon_min = (unsigned char)g->cLongitudeCent;

if (lat_deg > 90)  { if (out_err_type) strcpy(out_err_type, "GPS_LAT_DEG_OOB");
                     if (out_err_desc && err_desc_sz) snprintf(out_err_desc, err_desc_sz, "lat_deg=%u > 90", lat_deg);
                     return 0; }
if (lon_deg > 180) { if (out_err_type) strcpy(out_err_type, "GPS_LON_DEG_OOB");
                     if (out_err_desc && err_desc_sz) snprintf(out_err_desc, err_desc_sz, "lon_deg=%u > 180", lon_deg);
                     return 0; }
if (lat_min >= 60) { if (out_err_type) strcpy(out_err_type, "GPS_LAT_MIN_OOB");
                     if (out_err_desc && err_desc_sz) snprintf(out_err_desc, err_desc_sz, "lat_min=%u >= 60", lat_min);
                     return 0; }
if (lon_min >= 60) { if (out_err_type) strcpy(out_err_type, "GPS_LON_MIN_OOB");
                     if (out_err_desc && err_desc_sz) snprintf(out_err_desc, err_desc_sz, "lon_min=%u >= 60", lon_min);
                     return 0; }


    int lat_sign = dir_sign(g->cDirectionLatitude);
    int lon_sign = dir_sign(g->cDirectionLongitude);

    double lat1 = dms_to_deg((unsigned char)g->cLatitudeDegree,  (unsigned char)g->cLatitudeCent,  g->lLatitudeSec,  lat_sign);
    double lon1 = dms_to_deg((unsigned char)g->cLongitudeDegree, (unsigned char)g->cLongitudeCent, g->lLongitudeSec, lon_sign);

    int have_scaled = (g->FractionLen > 0) && (g->Latitude != 0 || g->Longitude != 0);
    double lat2 = 0.0, lon2 = 0.0;
    if (have_scaled) {
        lat2 = scaled_to_deg(g->Latitude,  g->FractionLen, lat_sign);
        lon2 = scaled_to_deg(g->Longitude, g->FractionLen, lon_sign);
    }

    double lat = have_scaled ? lat2 : lat1;
    double lon = have_scaled ? lon2 : lon1;

    if (have_scaled) {
        double dlat = fabs(lat2 - lat1);
        double dlon = fabs(lon2 - lon1);
        double max_diff = fmax(dlat, dlon);
        if (max_diff > 1e-5) { // 1e-5° ≈ 1.1 m
            if (out_err_type) strcpy(out_err_type, "GPS_MISMATCH");
            if (out_err_desc && err_desc_sz)
                snprintf(out_err_desc, err_desc_sz, "split vs scaled mismatch: dlat=%.7f, dlon=%.7f", dlat, dlon);
        }
    }

    unsigned char sats      = (unsigned char)g->reserved[0];
    unsigned char hdop_int  = (unsigned char)g->reserved[1];
    unsigned char hdop_frac = (unsigned char)g->reserved[2];
    int r95 = estimate_radius_r95_from_hdop(hdop_int, hdop_frac, sats, g->cGpsStatus);

    if (out_lat) *out_lat = lat;
    if (out_lon) *out_lon = lon;
    if (out_radius_m) *out_radius_m = r95;

    if (out_err_type && out_err_type[0] == '\0') strcpy(out_err_type, "null");
    if (out_err_desc && err_desc_sz && out_err_desc[0] == '\0') strcpy(out_err_desc, "null");
    return 1;
}

//计算角度（0-359）
static inline double deg2rad(double d) { return d * (M_PI / 180.0); }
static inline double rad2deg(double r) { return r * (180.0 / M_PI); }
static inline double clamp_heading_0_359(double deg) {
    double h = fmod(deg, 360.0);
    if (h < 0.0) h += 360.0;
    return h;
}
double haversine_m(double lat1_deg, double lon1_deg, double lat2_deg, double lon2_deg) {
    const double R = 6371000.0; // 地球半径（米）
    double lat1 = deg2rad(lat1_deg), lon1 = deg2rad(lon1_deg);
    double lat2 = deg2rad(lat2_deg), lon2 = deg2rad(lon2_deg);
    double dlat = lat2 - lat1, dlon = lon2 - lon1;
    double a = sin(dlat/2)*sin(dlat/2) + cos(lat1)*cos(lat2)*sin(dlon/2)*sin(dlon/2);
    double c = 2 * atan2(sqrt(a), sqrt(1.0 - a));
    return R * c;
}
double initial_bearing_deg(double lat1_deg, double lon1_deg, double lat2_deg, double lon2_deg) {
    double phi1 = deg2rad(lat1_deg), phi2 = deg2rad(lat2_deg);
    double dlon = deg2rad(lon2_deg - lon1_deg);
    double y = sin(dlon) * cos(phi2);
    double x = cos(phi1)*sin(phi2) - sin(phi1)*cos(phi2)*cos(dlon);
    double brg = rad2deg(atan2(y, x));           // [-180,180]
    return clamp_heading_0_359(brg);             // [0,360)
}

//base64处理
int read_file_all(const char *path, unsigned char **out_buf, size_t *out_len) {
    *out_buf = NULL; *out_len = 0;
    FILE *f = fopen(path, "rb");
    if (!f) return -1;
    if (fseek(f, 0, SEEK_END) != 0) { fclose(f); return -2; }
    long sz = ftell(f);
    if (sz < 0) { fclose(f); return -3; }
    if (fseek(f, 0, SEEK_SET) != 0) { fclose(f); return -4; }

    unsigned char *buf = (unsigned char*)malloc((size_t)sz);
    if (!buf) { fclose(f); return -5; }
    size_t n = fread(buf, 1, (size_t)sz, f);
    fclose(f);
    if (n != (size_t)sz) { free(buf); return -6; }

    *out_buf = buf;
    *out_len = n;
    return 0;
}

char *base64_encode(const unsigned char *data, size_t in_len, size_t *out_len) {
    static const char tbl[64] =
        "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    size_t olen = 4 * ((in_len + 2) / 3);
    char *out = (char*)malloc(olen + 1);
    if (!out) return NULL;

    size_t j = 0;
    for (size_t i = 0; i < in_len; i += 3) {
        unsigned int a = data[i];
        unsigned int b = (i + 1 < in_len) ? data[i + 1] : 0;
        unsigned int c = (i + 2 < in_len) ? data[i + 2] : 0;
        unsigned int triple = (a << 16) | (b << 8) | c;

        out[j++] = tbl[(triple >> 18) & 0x3F];
        out[j++] = tbl[(triple >> 12) & 0x3F];
        out[j++] = (i + 1 < in_len) ? tbl[(triple >> 6) & 0x3F] : '=';
        out[j++] = (i + 2 < in_len) ? tbl[triple & 0x3F] : '=';
    }
    out[j] = '\0';
    if (out_len) *out_len = j;
    return out;
}


//base解码
inline int b64_val(int ch) {
    if (ch == '=') return -2;
    if (isspace((unsigned char)ch)) return -3;
    if (ch >= 'A' && ch <= 'Z') return ch - 'A';
    if (ch >= 'a' && ch <= 'z') return ch - 'a' + 26;
    if (ch >= '0' && ch <= '9') return ch - '0' + 52;
    if (ch == '+' || ch == '-') return 62; 
    if (ch == '/' || ch == '_') return 63;
    return -1;
}

int base64_decode(const char *in_b64, unsigned char **out_buf, size_t *out_len) {
    if (!in_b64 || !out_buf || !out_len) return -1;

    size_t in_effective = 0;
    for (const char *p = in_b64; *p; ++p)
        if (!isspace((unsigned char)*p)) ++in_effective;
    if (in_effective == 0) { *out_buf = NULL; *out_len = 0; return 0; }

    size_t cap = (in_effective / 4) * 3 + 3; 
    unsigned char *buf = (unsigned char *)malloc(cap);
    if (!buf) return -1;

    int quartet[4], qi = 0;
    unsigned char *dp = buf;
    int finished = 0; 

    for (const char *p = in_b64; *p && !finished; ++p) {
        int v = b64_val((unsigned char)*p);
        if (v == -3) continue;             
        if (v == -1) { free(buf); return -1; } 

        quartet[qi++] = v;
        if (qi == 4) {
            int v0 = quartet[0], v1 = quartet[1], v2 = quartet[2], v3 = quartet[3];

            if (v2 == -2) {                  
                if (v3 != -2 || v0 < 0 || v1 < 0) { free(buf); return -1; }
                *dp++ = (unsigned char)((v0 << 2) | (v1 >> 4));
                qi = 0;
                finished = 1;               
            } else if (v3 == -2) {       
                if (v0 < 0 || v1 < 0 || v2 < 0) { free(buf); return -1; }
                *dp++ = (unsigned char)((v0 << 2) | (v1 >> 4));
                *dp++ = (unsigned char)((v1 << 4) | (v2 >> 2));
                qi = 0;
                finished = 1;               
            } else {
                if (v0 < 0 || v1 < 0 || v2 < 0 || v3 < 0) { free(buf); return -1; }
                *dp++ = (unsigned char)((v0 << 2) | (v1 >> 4));
                *dp++ = (unsigned char)((v1 << 4) | (v2 >> 2));
                *dp++ = (unsigned char)((v2 << 6) | v3);
                qi = 0;
            }
        }
    }

    if (!finished && qi != 0) {
        free(buf);
        return -1;
    }

    *out_buf = buf;
    *out_len = (size_t)(dp - buf);
    return 0;
}

char* normalize_b64(const char *in, size_t *out_len) {
    if (!in) return NULL;

    const char *p = in;
    const char *comma = strstr(p, "base64,");
    if (comma) p = comma + 7; 

    size_t cap = strlen(p) + 4;         
    char *buf = (char*)malloc(cap + 1);
    if (!buf) return NULL;

    size_t n = 0;
    for (; *p; ++p) {
        unsigned char c = (unsigned char)*p;
        if (c == '-') c = '+';          
        else if (c == '_') c = '/';

        if (isalnum(c) || c == '+' || c == '/') {
            buf[n++] = (char)c;
        } else if (c == '=') {
            buf[n++] = '=';
        } else {
            continue;
        }
    }

    size_t mod = n % 4;
    if (mod == 2) { buf[n++] = '='; buf[n++] = '='; }
    else if (mod == 3) { buf[n++] = '='; }

    buf[n] = '\0';
    if (out_len) *out_len = n;
    return buf;
}

int save_base64_to_file(const char *b64, const char *out_path) {
    if (!b64 || !out_path) return -1;

    int rc = -1;
    char *norm = NULL;
    unsigned char *bin = NULL;
    size_t norm_len = 0, bin_len = 0;

    norm = normalize_b64(b64, &norm_len);
    if (!norm || norm_len == 0) { rc = -2; goto done; }

    if (base64_decode(norm, &bin, &bin_len) != 0 || !bin || bin_len == 0) {
        rc = -3; goto done;
    }

    FILE *fp = fopen(out_path, "wb");
    if (!fp) { rc = -4; goto done; }
    size_t wr = fwrite(bin, 1, bin_len, fp);
    fclose(fp);
    if (wr != bin_len) { rc = -5; goto done; }

    rc = 0; // OK

done:
    if (norm) free(norm);
    if (bin) free(bin);
    return rc;
}

//GPS是否有效
void fill_location_status_from_gps(const SSYFI_GPS *gps, char *out, size_t out_sz)
{
    if (!out || out_sz == 0) return;

    const char *ret = "INVALID";
    if (gps) {
        unsigned int sats = (unsigned char)gps->reserved[0];  // 使用卫星数量
        int status_active = (gps->cGpsStatus == 'A' || gps->cGpsStatus == 1);
        int coords_ok = (gps->Latitude != 0 || gps->Longitude != 0);

        if (status_active) {
            ret = "AVTIVE";
        } else if (sats >= 4 && coords_ok) {
            ret = "AVTIVE";
        } else {
            ret = "INVALID";
        }
    }
    snprintf(out, out_sz, "%s", ret);
}

void join_image_ids_csv(const int *ids, int count, char *out, size_t out_size)
{
    if (!out || out_size == 0) return;
    out[0] = '\0';
    if (!ids || count <= 0) return;

    size_t pos = 0;
    for (int i = 0; i < count; ++i) {
        size_t cap = (pos < out_size) ? (out_size - pos) : 0;
        if (cap == 0) {
            out[out_size - 1] = '\0';
            break;
        }
        int n = snprintf(out + pos, cap, (i == 0) ? "%d" : ",%d", ids[i]);
        if (n < 0) {
            out[out_size - 1] = '\0';
            break;
        }
        if ((size_t)n >= cap) {
            break;
        }
        pos += (size_t)n;
    }
}


//GPS获取经度维度半径封装函数
int get_current_location_metrics(double *lat_deg, double *lon_deg,
                                        int *r95_m, int *direction_deg,
                                        char *err_type, char *err_desc, size_t err_desc_sz)
{
    if (!lat_deg || !lon_deg || !r95_m || !direction_deg ||
        !err_type || !err_desc) {
        return 0;
    }

    SSYFI_GPS gps;
    memset(&gps, 0, sizeof(gps));
    GetBackBoardGpsData(&gps);

    double lat = 0.0, lon = 0.0;
    int    r95 = 0;
    char   etype[64]  = {0};
    char   edesc[128] = {0};

    int ok = parse_gps_to_deg(&gps, &lat, &lon, &r95, etype, edesc, sizeof(edesc));

    static int    s_has_prev = 0;
    static double s_prev_lat = 0.0;
    static double s_prev_lon = 0.0;
    static int    s_prev_dir = 0;

    int dir_deg = s_prev_dir;

    if (ok) {
        if (s_has_prev) {
            double dist_m = haversine_m(s_prev_lat, s_prev_lon, lat, lon);
            if (dist_m >= 0.1) {  
                double brg = initial_bearing_deg(s_prev_lat, s_prev_lon, lat, lon);
                dir_deg = (int)lround(brg);
                if (dir_deg >= 360) dir_deg = 0;
                s_prev_dir = dir_deg;
            }
        } else {
            s_prev_dir = dir_deg;
            s_has_prev = 1;
        }
        s_prev_lat = lat;
        s_prev_lon = lon;
    } else {
    }

    *lat_deg       = lat;
    *lon_deg       = lon;
    *r95_m         = r95;
    *direction_deg = dir_deg;

    SAFE_STRCPY(err_type, etype);
    SAFE_STRCPY(err_desc, edesc);

    return ok ? 1 : 0;
}


int kitid_validate_then_fill(const char *reqKidIn, const char *storedKidIn)
{
    char reqKid[64]    = {0};
    char storedKid[64] = {0};
    SAFE_STRCPY_ARR(reqKid,    reqKidIn);
    SAFE_STRCPY_ARR(storedKid, storedKidIn);

    if (reqKid[0] == '\0' || storedKid[0] == '\0' || strcmp(reqKid, storedKid) != 0) {
        return 0;
    }

    return 1;
}

//对ids_csv计数
int count_ids_csv(const char *s) {
    if (!s) return 0;
    while (*s && isspace((unsigned char)*s)) s++;
    if (*s == '\0') return 0;

    int cnt = 0;
    const char *p = s;
    int in_num = 0;
    while (*p) {
        if (isdigit((unsigned char)*p)) {
            if (!in_num) { cnt++; in_num = 1; }
        } else if (*p == ',') {
            in_num = 0;
        } else if (isspace((unsigned char)*p)) {
        } else {
            in_num = 0;
        }
        p++;
    }
    return cnt;
}

//将传进的ids_csv，将chs 设成 0..n-1 并把 chs_cnt 设为 n ids_csd = "1,2,3"->{1，2，3}
void build_channels_from_ids(const char *ids_csv, int *chs, int *chs_cnt, int chs_cap)
{
    int n = count_ids_csv(ids_csv);
    if (n <= 0) {
        if (chs && chs_cap > 0) chs[0] = 0;
        if (chs_cnt) *chs_cnt = 1;
        return;
    }
    if (n > chs_cap) n = chs_cap;  

    for (int i = 0; i < n; ++i) chs[i] = i;
    if (chs_cnt) *chs_cnt = n;
}

#ifdef __cplusplus
}
#endif

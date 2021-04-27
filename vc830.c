/*
 * =============================================================================
 *             (c) by Thomas Welsch / 2021 under the MIT License
 * =============================================================================
 *
 * This file implemnts a Voltcraft VC-830 serial protocol decoder.
 * Based on datasheet FS9922-DMM4-DS-15_EN.pdf.
 * 
 * CLANG Formater style:
 * { BasedOnStyle: Google, IndentWidth: 4, ColumnLimit: 0, AlignConsecutiveAssignments: true, AlignConsecutiveMacros: true, AlignConsecutiveDeclarations: true, AlignOperands: true, AllowShortBlocksOnASingleLine: true, AllowShortIfStatementsOnASingleLine: true, AllowShortLoopsOnASingleLine: true, KeepEmptyLinesAtTheStartOfBlocks: true, BreakBeforeBraces: Linux }
 */

#include <ctype.h>
#include <fcntl.h>
#include <limits.h>
#include <pthread.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/time.h>
#include <termios.h>
#include <unistd.h>

// --------------------------------------------------------------------------------------------------------------

#define VERSION "1.0.0"

#define BUFFER_LEN          100     // Used for all kind of static allocations...
#define END_OF_CAPTURE_FILE 142857  // EOF of test file reached

typedef unsigned char byte;

#define strequal(s1, s2) (strcmp((s1), (s2)) == 0)

struct Vc830 {
    struct timeval receivedAt;                   // Time with mills
    char           rawRisplay[6];                // 0.000
    char           sign;                         // -/+
    char           mode[BUFFER_LEN];             // AC/DC/AC HOLD/AC REL...
    char           unit[BUFFER_LEN];             // A/V...
    char           prefix[BUFFER_LEN];           // m, µ, K....
    char           fullUnit[BUFFER_LEN];         // combined units,  mA,V, mV...
    char           info[BUFFER_LEN];             // Other infos, not fits currently to the other fields
    int            barGraph;                     // Bar graph level in %, 0..60, above 60 the autorange is switing to the next upper range
    bool           barGraphIsShown;              // The bar graph is displayed
    bool           batteryWarning;               // Battery warning is diesplayed
    bool           autoRangeActive;              // Auto range is active
    bool           holdActive;                   // Measurement on holdwith hold key
    bool           deltaActive;                  // Measurement switch to REL/DELTA mode. no absolute value!
    bool           overflow;                     // Overflow, mostly if autorange is switched off
    char           formatedValue[BUFFER_LEN];    // Formated value with measurement unit
    char           formatedSiValue[BUFFER_LEN];  // Formated value, normed to SI base unit
};

// --------------------------------------------------------------------------------------------------------------

void exitWithError(const char* message)
{
    fprintf(stderr, "Error: Program exit with: %s", message);
    exit(-1);
}

// --------------------------------------------------------------------------------------------------------------

void showUsageAndExit(const char* message)
{
    if (message && *message)
        fprintf(stderr, "Error: %s\n", message);

    fprintf(stderr, "vc830: Reads serial data from a RS232 device for Voltcraft VC830 DMM. (c) 2021 version %s, Thomas Welsch (ttww@gmx.de)\n", VERSION);

    fprintf(stderr, "Usage: vc830 [-f output-format] [-t time-format] [-c count] <tty device where the VC830 is connected>.\n");
    fprintf(stderr, "              -f   output-format  keyvalue, json, human, si.           Default = human\n");
    fprintf(stderr, "              -t   time-format    iso, local, epochsecms, human, none  Default = none\n");
    fprintf(stderr, "              -c   count          number of samples                    Default = endless\n");

    exit(-1);
}

// --------------------------------------------------------------------------------------------------------------

void setRtsDtr(int fd)
{
    int arg = TIOCM_RTS | TIOCM_DTR;
    ioctl(fd, TIOCMBIS, &arg);
    arg = TIOCM_RTS;
    ioctl(fd, TIOCMBIC, &arg);
}

// --------------------------------------------------------------------------------------------------------------

int openDevice(const char* deviceName)
{
    speed_t bdflag = B2400;  // All VC-830 with FS9922-DMM4 chips are using 2400 Baud, 8N1

    static struct termios tty;
    int                   modelines = 0;

    int fd = open(deviceName, O_RDWR | O_NOCTTY | O_NDELAY);

    if (fd < 0) {
        perror("open failed");
        exit(-1);
    }
    if (isatty(fd)) {
        tcgetattr(fd, &tty);
        tty.c_iflag     = IGNBRK | IGNPAR;
        tty.c_lflag     = 0;
        tty.c_oflag     = 0;                             // raw output
        tty.c_cc[VTIME] = 0;                             // no waiting
        tty.c_cc[VMIN]  = 1;                             // minimum of reading bytes, was 1
        tty.c_cflag     = CS8 | CREAD | CLOCAL | HUPCL;  // was STD_FLG; 8N1, read, no modem status,
#ifdef __linux__
        tty.c_line = 0;
#endif

        cfsetispeed(&tty, bdflag);  // input
        cfsetospeed(&tty, bdflag);  // output

        if (tcsetattr(fd, TCSAFLUSH, &tty) == -1) {
            perror("tcsetattr TCSAFLUSH failed");
            return (-1);
        }

        if (ioctl(fd, TIOCEXCL, &modelines) == -1)  // Put the tty into exclusive mode.
        {
            perror("ioctl TIOCEXCL failed");
            return (-1);
        }

        setRtsDtr(fd);           // DTR/RTS, important for voltage supply to RS232 optical converter
        tcflush(fd, TCIOFLUSH);  // flush buffers
    } else {
        //fprintf(stderr, "The Device %s is not a terminal device, reading captured data !\n", deviceName);
    }

    return (fd);
}

// --------------------------------------------------------------------------------------------------------------

char* to_binary(unsigned char x)
{
    static char b[9];
    b[8] = '\0';

    int idx = 0;
    for (int z = 128; z > 0; z >>= 1) {
        b[idx++] = ((x & z) == z) ? '1' : '0';
    }

    return b;
}

// --------------------------------------------------------------------------------------------------------------

void showBuffer(byte buf[])
{
    for (int n = 0; n < 14; n++) fprintf(stderr, "   %-2d    ", n);
    fprintf(stderr, "\n");

    for (int n = 0; n < 14; n++) fprintf(stderr, "%s ", to_binary(buf[n]));
    fprintf(stderr, "\n");

    for (int n = 0; n < 14; n++) fprintf(stderr, "  %02x     ", buf[n]);
    fprintf(stderr, "\n");

    for (int n = 0; n < 14; n++) fprintf(stderr, " %3c     ", isalnum(buf[n]) ? buf[n] : '?');
    fprintf(stderr, "\n");
    fprintf(stderr, "\n");
}

// --------------------------------------------------------------------------------------------------------------

void strinsert(char* srcAndDest, int pos, const char* toInsert)
{
    char* buf = malloc(strlen(srcAndDest) + strlen(toInsert) + 1);

    strncpy(buf, srcAndDest, pos);
    buf[pos] = '\0';
    strcat(buf, toInsert);
    strcat(buf, srcAndDest + pos);
    strcpy(srcAndDest, buf);
    free(buf);
}

// --------------------------------------------------------------------------------------------------------------

bool checkInfo(byte buf[], int statusByte, int statusBit, const char* info, char* out)
{
    if (buf[7 + statusByte - 1] & (1 << statusBit)) {
        if (out) {
            if (out[0]) strcat(out, " ");
            strcat(out, info);
        }
        return true;
    }
    return false;
}

// --------------------------------------------------------------------------------------------------------------

void trimZeros(char* s)
{
    char* e = strchr(s, '\0');
    while (e > s) {
        e--;
        if (*e != '0') return;
        if (e > s && *(e - 1) == '.') return;  // keep last zero after point
        *e = '\0';
    }
}

// --------------------------------------------------------------------------------------------------------------

int decodeFS9922Paket(byte buf[], struct Vc830* vc830Data)
{
    //showBuffer(buf);

    memset(vc830Data, 0, sizeof(*vc830Data));

    gettimeofday(&vc830Data->receivedAt, NULL);

    // Check space and CRLF
    if (buf[5] != 0x20 || buf[12] != 0x0d || buf[13] != 0x0a)
        return -1;

    // Check sign
    int sign = 0;
    if (buf[0] == 0x2b) sign = 1;
    if (buf[0] == 0x2d) sign = -1;

    if (sign == 0) return -2;

    // Check value/digits
    char value[6];
    int  k = -1;

    if (buf[1] == 0x3f && buf[2] == 0x30 && buf[3] == 0x3a && buf[4] == 0x3f) {
        vc830Data->overflow = true;
        strcpy(value, "OVF");
    } else {
        // Read value
        for (int i = 0; i < 4; i++) {
            if (!isdigit(buf[1 + i])) return -3;
            value[i] = buf[1 + i];
        }
        value[4] = '\0';

        // if (buf[6] == 0x30) k = 0;
        if (buf[6] == 0x31) k = 1;
        if (buf[6] == 0x32) k = 2;
        if (buf[6] == 0x33) k = 3;
        if (buf[6] == 0x34) k = 3;

        // if (buf[6] == 0x34) nk = 1;
        if (k != -1) strinsert(value, k, ".");
    }

    // Check status bytes
    char mode[BUFFER_LEN];
    char prefix[BUFFER_LEN];
    char unit[BUFFER_LEN];
    char info[BUFFER_LEN];

    mode[0]   = '\0';
    prefix[0] = '\0';
    unit[0]   = '\0';
    info[0]   = '\0';

    // Status byte SB1:
    vc830Data->autoRangeActive = checkInfo(buf, 1, 5, "AUTO", info);
    checkInfo(buf, 1, 4, "DC", mode);
    checkInfo(buf, 1, 3, "AC", mode);
    vc830Data->deltaActive     = checkInfo(buf, 1, 2, "REL", mode);  // Delta
    vc830Data->holdActive      = checkInfo(buf, 1, 1, "HOLD", mode);
    vc830Data->barGraphIsShown = checkInfo(buf, 1, 0, "BPN", NULL);  // Bargraph is shown, info not relevant

    // Status byte SB2:
    checkInfo(buf, 2, 7, "Diode" /*"Z1"*/, info);  // Diode Unit Volt oder sperre
    checkInfo(buf, 2, 6, "Z2", info);
    checkInfo(buf, 2, 5, "MAX", info);
    checkInfo(buf, 2, 4, "MIN", info);
    checkInfo(buf, 2, 3, "APO", info);
    vc830Data->batteryWarning = checkInfo(buf, 2, 2, "Bat", info);
    checkInfo(buf, 2, 1, "n", prefix);
    checkInfo(buf, 2, 0, "Z3", info);

    // Status byte SB3:
    checkInfo(buf, 3, 7, "µ", prefix);
    checkInfo(buf, 3, 6, "m", prefix);
    checkInfo(buf, 3, 5, "k", prefix);
    checkInfo(buf, 3, 4, "M", prefix);
    checkInfo(buf, 3, 3, "Beep", info);  // Durchgangsprüfung BEEPER
    checkInfo(buf, 3, 2, "Diode", info);
    checkInfo(buf, 3, 1, "%", prefix);  // Duty for HZ
    checkInfo(buf, 3, 0, "Z4", info);

    // Status byte SB4:
    checkInfo(buf, 4, 7, "V", unit);
    checkInfo(buf, 4, 6, "A", unit);
    checkInfo(buf, 4, 5, "Ω", unit);
    checkInfo(buf, 4, 4, "hFE", unit);
    checkInfo(buf, 4, 3, "Hz", unit);
    checkInfo(buf, 4, 2, "F", unit);
    checkInfo(buf, 4, 1, "°C", unit);
    checkInfo(buf, 4, 0, "°F", unit);

    double multToSi = 1;
    if (strequal(prefix, "n")) multToSi = 0.000000001;
    if (strequal(prefix, "µ")) multToSi = 0.000001;
    if (strequal(prefix, "m")) multToSi = 0.001;
    if (strequal(prefix, "k")) multToSi = 1000;
    if (strequal(prefix, "k")) multToSi = 1000000;

    // Bar % (0-60)
    byte bar = buf[11] & 0x7f;  // Hi-Bit is sign

    // For the none normalized values (comming from the digits) we keep the
    // resolution. For the SI base unit normalized values, we scrap the trailing
    // '0', even if the are comming from the display...
    double vSi = atof(value) * multToSi * sign;

    char vWithUnit[20];
    char vSiWithUnit[20];

    vWithUnit[0] = '\0';
    if (sign == -1) strcat(vWithUnit, "-");

    char* vz = value;
    while (*vz == '0' && *(vz + 1) != '.') vz++;

    strcat(vWithUnit, vz);
    strcat(vWithUnit, " ");
    strcat(vWithUnit, prefix);
    strcat(vWithUnit, unit);

    snprintf(vSiWithUnit, sizeof(vSiWithUnit), "%f", vSi);
    trimZeros(vSiWithUnit);
    strcat(vSiWithUnit, " ");
    strcat(vSiWithUnit, unit);

    if (false) printf(
        "abs value = |%s| --> sign %d k %d  mode |%s| prefix |%s| unit |%s| info |%s| bar = %d | v = %s  vSi = %f (%s)\n",
        value, sign, k, mode, prefix, unit, info, bar, vWithUnit, vSi, vSiWithUnit);

    // Copy to output structure
    vc830Data->sign     = sign == 1 ? '+' : '-';
    vc830Data->barGraph = bar;

    strcpy(vc830Data->rawRisplay, value);
    strcpy(vc830Data->mode, mode);
    strcpy(vc830Data->info, info);
    strcpy(vc830Data->unit, unit);
    strcpy(vc830Data->prefix, prefix);
    strcpy(vc830Data->fullUnit, prefix);
    strcat(vc830Data->fullUnit, unit);
    strcpy(vc830Data->formatedValue, vWithUnit);
    strcpy(vc830Data->formatedSiValue, vSiWithUnit);

    return 0;
}

// --------------------------------------------------------------------------------------------------------------

int read14BytesPaket(int fd, byte readBuffer[])
{
    fd_set         set;
    struct timeval timeout;
    int            bufIdx = 0;

    while (1) {
        timeout.tv_sec  = 0;
        timeout.tv_usec = 100000;

        FD_ZERO(&set);    /* clear the set */
        FD_SET(fd, &set); /* add our file descriptor to the set */

        int ret = select(fd + 1, &set, NULL, NULL, &timeout);
        if (ret == -1) {
            perror("select failed");
            return -1;
        }

        if (ret == 0)  // timeout, reset buffer
        {
            bufIdx = 0;
            continue;
        }

        int l = read(fd, &readBuffer[bufIdx], 1);
        if (l == 0) {
            // This happens only if we are reading a file. For normal TTY devices
            // the above select() returns only if we have data for the read() call.
            return END_OF_CAPTURE_FILE;
        }
        if (l > 0) bufIdx += l;

        if (bufIdx == 14) return 0;

    }  // while (1)
}

// --------------------------------------------------------------------------------------------------------------

char* getLocalTime(struct timeval t)
{
    static char ret[64];
    struct tm*  nowtm;
    char        tmbuf[32];

    nowtm = localtime(&t.tv_sec);
    strftime(tmbuf, sizeof(tmbuf), "%H:%M:%S", nowtm);

    snprintf(ret, sizeof(ret), "%s.%03ld", tmbuf, (long)t.tv_usec / 1000);  // tv_usec is int on some platforms

    return ret;
}

// --------------------------------------------------------------------------------------------------------------

char* getLocalDateTime(struct timeval t)
{
    static char ret[64];
    struct tm*  nowtm;

    nowtm = localtime(&t.tv_sec);

    strftime(ret, sizeof(ret), "%Y-%m-%d %H:%M:%S", nowtm);

    return ret;
}

// --------------------------------------------------------------------------------------------------------------

char* getIso8601Time(struct timeval t)
{
    static char ret[200];
    struct tm*  nowtm;
    char        tmbuf[64];
    char        tzbuf[64];

    nowtm = localtime(&t.tv_sec);
    strftime(tmbuf, sizeof tmbuf, "%Y-%m-%dT%H:%M:%S", nowtm);
    strftime(tzbuf, sizeof tzbuf, "%z", nowtm);
    snprintf(ret, sizeof(ret), "%s.%06ld%s", tmbuf, (long)t.tv_usec, tzbuf);  // tv_usec is int on some platforms

    return ret;
}

// --------------------------------------------------------------------------------------------------------------

char* getEpochSecMsTime(struct timeval t)
{
    static char ret[64];
    snprintf(ret, sizeof(ret), "%ld.%ld", t.tv_sec, (long)t.tv_usec);
    return ret;
}

// --------------------------------------------------------------------------------------------------------------

void outputJsonString(const char* key, const char* value)
{
    fprintf(stdout, "\t\"%s\": \"%s\"", key, value);
}
void outputJsonChar(const char* key, char value)
{
    fprintf(stdout, "\t\"%s\": \"%c\"", key, value);
}
void outputJsonInt(const char* key, int value)
{
    fprintf(stdout, "\t\"%s\": %d", key, value);
}
void outputJsonBool(const char* key, bool value)
{
    fprintf(stdout, "\t\"%s\": %s", key, value ? "true" : "false");
}
void outputJsonTimestamp(const char* key, struct timeval value)
{
    fprintf(stdout, "\t\"%s\": \"%s\"", key, getIso8601Time(value));
}
void outputJsonNLSEP()
{
    fprintf(stdout, ",\n");
}
void outputJsonNL()
{
    fprintf(stdout, "\n");
}

// --------------------------------------------------------------------------------------------------------------

void outputKvString(const char* key, const char* value)
{
    fprintf(stdout, "%s=%s\n", key, value);
}
void outputKvChar(const char* key, char value)
{
    fprintf(stdout, "%s=%c\n", key, value);
}
void outputKvInt(const char* key, int value)
{
    fprintf(stdout, "%s=%d\n", key, value);
}
void outputKvBool(const char* key, bool value)
{
    fprintf(stdout, "%s=%s\n", key, value ? "true" : "false");
}
void outputKvTimestamp(const char* key, struct timeval value)
{
    fprintf(stdout, "%s=%s\n", key, getIso8601Time(value));
}

// --------------------------------------------------------------------------------------------------------------

void showDataKeyValue(struct Vc830* vc830Data, const char* timeText)
{
    outputKvTimestamp("receivedAt", vc830Data->receivedAt);
    if (*timeText) outputKvString("receivedAtFormated", timeText);
    outputKvChar("sign", vc830Data->sign);
    outputKvString("mode", vc830Data->mode);
    outputKvString("unit", vc830Data->unit);
    outputKvString("prefix", vc830Data->prefix);
    outputKvString("fullUnit", vc830Data->fullUnit);
    outputKvString("info", vc830Data->info);
    outputKvInt("barGraph", vc830Data->barGraph);
    outputKvBool("barGraphIsShown", vc830Data->barGraphIsShown);
    outputKvBool("batteryWarning", vc830Data->batteryWarning);
    outputKvBool("autoRangeActive", vc830Data->autoRangeActive);
    outputKvBool("holdActive", vc830Data->holdActive);
    outputKvBool("deltaActive", vc830Data->deltaActive);
    outputKvBool("overflow", vc830Data->overflow);
    outputKvString("rawRisplay", vc830Data->rawRisplay);
    outputKvString("formatedValue", vc830Data->formatedValue);
    outputKvString("formatedSiValue", vc830Data->formatedSiValue);
}

// --------------------------------------------------------------------------------------------------------------

void showDataJson(struct Vc830* vc830Data, const char* timeText)
{
    fprintf(stdout, "{\n");

    outputJsonTimestamp("receivedAt", vc830Data->receivedAt);
    outputJsonNLSEP();
    if (*timeText) {
        outputJsonString("receivedAtFormated", timeText);
        outputJsonNLSEP();
    }
    outputJsonChar("sign", vc830Data->sign);
    outputJsonNLSEP();
    outputJsonString("mode", vc830Data->mode);
    outputJsonNLSEP();
    outputJsonString("unit", vc830Data->unit);
    outputJsonNLSEP();
    outputJsonString("prefix", vc830Data->prefix);
    outputJsonNLSEP();
    outputJsonString("fullUnit", vc830Data->fullUnit);
    outputJsonNLSEP();
    outputJsonString("info", vc830Data->info);
    outputJsonNLSEP();
    outputJsonInt("barGraph", vc830Data->barGraph);
    outputJsonNLSEP();
    outputJsonBool("barGraphIsShown", vc830Data->barGraphIsShown);
    outputJsonNLSEP();
    outputJsonBool("batteryWarning", vc830Data->batteryWarning);
    outputJsonNLSEP();
    outputJsonBool("autoRangeActive", vc830Data->autoRangeActive);
    outputJsonNLSEP();
    outputJsonBool("holdActive", vc830Data->holdActive);
    outputJsonNLSEP();
    outputJsonBool("deltaActive", vc830Data->deltaActive);
    outputJsonNLSEP();
    outputJsonBool("overflow", vc830Data->overflow);
    outputJsonNLSEP();
    outputJsonString("rawRisplay", vc830Data->rawRisplay);
    outputJsonNLSEP();
    outputJsonString("formatedValue", vc830Data->formatedValue);
    outputJsonNLSEP();
    outputJsonString("formatedSiValue", vc830Data->formatedSiValue);
    outputJsonNL();

    fprintf(stdout, "}\n");
}

// --------------------------------------------------------------------------------------------------------------

void showDataHuman(struct Vc830* vc830Data, const char* timeText)
{
    fprintf(stdout, "%s%s%s\t\t%s\t%s\n", timeText, *timeText ? "\t\t" : "", vc830Data->formatedValue, vc830Data->mode, vc830Data->info);
}

// --------------------------------------------------------------------------------------------------------------

void showDataSi(struct Vc830* vc830Data, const char* timeText)
{
    fprintf(stdout, "%s%s%s\t\t%s\t%s\n", timeText, *timeText ? "\t\t" : "", vc830Data->formatedSiValue, vc830Data->mode, vc830Data->info);
}

// --------------------------------------------------------------------------------------------------------------

void showData(struct Vc830* vc830Data, const char* outputFormat, const char* timeFormat)
{
    char* timeText = NULL;
    // iso, local, epochsecms, human, none
    if (strequal(timeFormat, "iso")) {
        timeText = getIso8601Time(vc830Data->receivedAt);
    }
    if (strequal(timeFormat, "local")) {
        timeText = getLocalDateTime(vc830Data->receivedAt);
    }
    if (strequal(timeFormat, "human")) {
        timeText = getLocalTime(vc830Data->receivedAt);
    }
    if (strequal(timeFormat, "epochsecms")) {
        timeText = getEpochSecMsTime(vc830Data->receivedAt);
    }
    if (strequal(timeFormat, "none")) {
        timeText = "";
    }

    if (timeText == NULL) showUsageAndExit("Unknown time format.");

    // keyvalue, json, human, si
    if (strequal(outputFormat, "keyvalue")) {
        showDataKeyValue(vc830Data, timeText);
        return;
    }
    if (strequal(outputFormat, "json")) {
        showDataJson(vc830Data, timeText);
        return;
    }
    if (strequal(outputFormat, "human")) {
        showDataHuman(vc830Data, timeText);
        return;
    }
    if (strequal(outputFormat, "si")) {
        showDataSi(vc830Data, timeText);
        return;
    }
}

// --------------------------------------------------------------------------------------------------------------

int main(int argc, char** argv)
{
    int ret;

    //
    // Parse arguments
    //
    char outputFormat[BUFFER_LEN];
    char timeFormat[BUFFER_LEN];
    long count = LONG_MAX;  // Almost endless :-)
    char deviceName[PATH_MAX];

    strcpy(outputFormat, "human");
    strcpy(timeFormat, "none");
    strcpy(deviceName, "");

    for (int i = 1; i < argc; i++) {
        if (i < argc - 2) {
            if (strequal(argv[i], "-f")) {
                strcpy(outputFormat, argv[i + 1]);
                i++;
                continue;
            }
            if (strequal(argv[i], "-t")) {
                strcpy(timeFormat, argv[i + 1]);
                i++;
                continue;
            }
            if (strequal(argv[i], "-c")) {
                count = atol(argv[i + 1]);
                i++;
                continue;
            }
        }

        strncpy(deviceName, argv[i], sizeof(deviceName) - 1);
        deviceName[sizeof(deviceName) - 1] = '\0';
        break;  // must be the last parameter
    }
    if (!deviceName[0]) showUsageAndExit("Missing instrument device.");

    //
    // Open device or captured file
    //
    int fd = openDevice(deviceName);
    if (fd < 0) exitWithError("Open device failed");

    //
    // Loop over device reads
    //
    byte         readBuffer[BUFFER_LEN];
    struct Vc830 vc830Data;
    long         loop = 0;

    while (loop < count) {
        loop++;

        ret = read14BytesPaket(fd, readBuffer);
        if (ret == END_OF_CAPTURE_FILE) break;  // --> terminate
        if (ret != 0) exitWithError("Read failed");

        ret = decodeFS9922Paket(readBuffer, &vc830Data);
        if (ret != 0) {
            fprintf(stderr, "VC-830 data paket parsing failed with %d\n", ret);
            continue;
        }

        showData(&vc830Data, outputFormat, timeFormat);
    }  // while

    close(fd);
    exit(0);
}

#define _GNU_SOURCE

#include <unistd.h>
#include <stdint.h>
#include <stdlib.h>
#include <stdio.h>
#include <limits.h>

#include <sys/types.h>
#include <sys/stat.h>
#include <sys/ioctl.h>
#include <fcntl.h>
#include <poll.h>

#include <sched.h>
#include <sys/mman.h>

#include <string.h>
#include <errno.h>
#include <linux/gpio.h>
#include <time.h>

#include <getopt.h>


/* Infered from: uapi/linux/gpio.h */
#define MAX_EVENTS ((GPIO_V2_LINES_MAX) * 16)


/************************************************************************
 * Raspberry PI GPIO defintions                                         *
 ************************************************************************/

/* RPI gpio chipset */
#define RPI_GPIO_CHIP		"gpiochip0"

/* RPI pin mapping (-1 are power or ground pin) */
static int rpi_pinmap[] = {
    -1, -1,  2, -1,  3, -1,  4, 14, -1, 15,
    17, 18, 27, -1, 22, 23, -1, 24, 10, -1,
     9, 25, 11,  8, -1,  7,  0,  1,  5, -1,
     6, 12, 13, -1, 19, 16, 26, 20, -1, 21 };


/************************************************************************
 * Log and debug                                                        *
 ************************************************************************/

#ifndef WITH_LOG
#define LOG(x, ...)
#endif

#ifndef LOG
#include <stdio.h>
#include <errno.h>
#define LOG(x, ...) do {						\
	int errno_saved = errno;					\
	fprintf(stderr, x "\n", ##__VA_ARGS__);				\
	errno = errno_saved;						\
    } while(0)
#endif

#ifndef LOG_ERRNO
#define LOG_ERRNO(x, ...)						\
	LOG(x " (%s)", ##__VA_ARGS__, strerror(errno))
#endif

#ifndef ASSERT
#include <assert.h>
#define ASSERT(x)							\
    assert(x)
#endif


/************************************************************************
 * Output                                                               *
 ************************************************************************/

#ifndef PUT
#define PUT(x, ...) do {						\
	int errno_saved = errno;					\
	struct timespec ts;						\
	clock_gettime(CLOCK_TAI, &ts);					\
	fprintf(stdout, "%ld.%06ld: " x "\n",				\
		ts.tv_sec, ts.tv_nsec/1000, ##__VA_ARGS__);		\
	errno = errno_saved;						\
    } while(0)
#endif


/************************************************************************
 * Argument line parsing                                                *
 ************************************************************************/

#define USAGE_DIE(x, ...) do {						\
	fprintf(stderr, x "\n", ##__VA_ARGS__);				\
	exit(1);							\
    } while(0)


struct config {
    struct {                     // Controller
	char    *id;             //  - identifier
	int      fd;             //  - file descriptor
    } ctrl;
    struct {                     // Pin
	uint32_t id;             //  - identifier
	int      fd;             //  - file descriptor
	uint64_t flags;          //  - flags
	char    *label;          //  - label
    } pin;
    struct {                     // Flags
	uint8_t debounce    :1;  //   - debounce
	uint8_t idle_timeout:1;  //   - idel timeout
    } flags;
    uint32_t debounce;           // debounce time in µs
    uint64_t idle_timeout;       // idle timeout in µs
};

static int
parse_period(const char *option, uint64_t *val)
{
    errno = 0;
    char    *end = NULL;
    uint64_t   v = strtoul(option, &end, 10);
    if (errno != 0) return -1;

    if      (strcmp(end, "us" ) == 0) { v *=          1ull; } 
    else if (strcmp(end, "ms" ) == 0) { v *=       1000ull; }
    else if (strcmp(end, "s"  ) == 0) { v *=    1000000ull; }
    else if (strcmp(end, "min") == 0) { v *=   60000000ull; }
    else if (strcmp(end, "h"  ) == 0) { v *= 3600000000ull; }
    else                              { return -1;          }
    
    *val = v;
    return 0;
}

static int
parse_debounce(const char *option, uint32_t *val)
{
    uint64_t v;
    if ((parse_period(option, &v) < 0            ) ||
	(v                        > UINT32_MAX   ) ||
	(v                        > 3600000000ull))
	return -1;

    *val = v;
    return 0;
}

static int
parse_idle_timeout(const char *option, uint64_t *val)
{
    uint64_t v;
    if ((parse_period(option, &v) < 0              ) ||
	(v                        > UINT64_MAX     ) ||
	(v                        > 172800000000ull))
	return -1;

    *val = v;
    return 0;
}


static int
parse_edge(const char *option, uint64_t *flags)
{
    static const uint64_t all_flags =
	GPIO_V2_LINE_FLAG_EDGE_RISING |
	GPIO_V2_LINE_FLAG_EDGE_FALLING;

    if        (strcmp(option, "rising" ) == 0) {
	*flags &= ~all_flags;
	*flags |= GPIO_V2_LINE_FLAG_EDGE_RISING;
    } else if (strcmp(option, "falling") == 0) {
	*flags &= ~all_flags;
	*flags |= GPIO_V2_LINE_FLAG_EDGE_FALLING;
    } else {
	return -1;
    }
    return 0;
}

static int
parse_bias(const char *option, uint64_t *flags)
{
    static const uint64_t all_flags =
	GPIO_V2_LINE_FLAG_BIAS_DISABLED |
	GPIO_V2_LINE_FLAG_BIAS_PULL_UP  |
	GPIO_V2_LINE_FLAG_BIAS_PULL_DOWN;
    
    if        (strcmp(option, "as-is"    ) == 0) {
	*flags &= ~all_flags;
    } else if (strcmp(option, "disabled" ) == 0) {
	*flags &= ~all_flags;
	*flags |= GPIO_V2_LINE_FLAG_BIAS_DISABLED;
    } else if (strcmp(option, "pull-up"  ) == 0) {
	*flags &= ~all_flags;
	*flags |= GPIO_V2_LINE_FLAG_BIAS_PULL_UP;
    } else if (strcmp(option, "pull-down") == 0) {
	*flags &= ~all_flags;
	*flags |= GPIO_V2_LINE_FLAG_BIAS_PULL_DOWN;
    } else {
	return -1;
    }
    return 0;
}

static int
parse_gpio(const char *option, char **chip_id, uint32_t *pin_id)
{
    char *sep = strchr(option, ':');
    if (sep == NULL) return -1;

    char *_chip_id = strndup(option, sep - option);
    if (_chip_id == NULL) return -1;
    
    char   * endptr   = NULL;
    char   * number   = sep + 1;
    uint32_t _pin_id  = strtoul(number, &endptr, 10);

    if ((number[0] == '\0') || (endptr[0] != '\0'))
	goto failed;

    if (strcmp(_chip_id, "rpi") == 0) {
	free(_chip_id);
	_chip_id = strdup(RPI_GPIO_CHIP);
	if (_chip_id == NULL) return -1;
	if ((_pin_id < 1) || (_pin_id > 40)) return -1;
	if (rpi_pinmap[_pin_id - 1] < 0) return -1;
	_pin_id = rpi_pinmap[_pin_id - 1];
    }

    return 0;
    
 failed:
    free(_chip_id);
    return -1;
}

static void
parse_config(int argc, char **argv, struct config *cfg)
{
    static const char *const shortopts = "+L:D:I:b:e:h";
    
    const struct option longopts[] = {
	{ "label",	     required_argument, NULL,	'L' },
	{ "debounce",        required_argument, NULL,	'D' },
	{ "idle-timeout",    required_argument, NULL,	'I' },
	{ "bias",            required_argument, NULL,	'b' },
	{ "edge",            required_argument, NULL,	'e' },
	{ "help",	     no_argument,	NULL,	'h' },
	{ NULL },
    };

    int opti, optc;
    
    for (;;) {
	optc = getopt_long(argc, argv, shortopts, longopts, &opti);
	if (optc < 0)
	    break;
	
	switch (optc) {
	case 'L':
	    cfg->pin.label = optarg;
	    break;
	case 'D':
	    if (parse_debounce(optarg, &cfg->debounce) < 0)
		USAGE_DIE("invalid debounce time (1us .. 1h)");
	    cfg->flags.debounce = 1;
	    break;
	case 'I':
	    if (parse_idle_timeout(optarg, &cfg->idle_timeout) < 0)
		USAGE_DIE("invalid idle timeout (1us .. 48h)");
	    cfg->flags.idle_timeout = 1;
	    break;
	case 'e':
	    if (parse_edge(optarg, &cfg->pin.flags) < 0)
		USAGE_DIE("invalid edge (rising, failing)");
	    break;
	case 'b':
	    if (parse_bias(optarg, &cfg->pin.flags) < 0)
		USAGE_DIE("invalid bias (as-is, disabled, pull-up, pull-down)");
	    break;
	case 'h':
	    printf("pulse-counting [opts] chipset:pin\n");
	    exit(0);
	case 0:
	    break;
	default:
	    abort();
	}
    }


    argc -= optind;
    argv += optind;

    if (argc != 1)
	USAGE_DIE("missing GPIO pin (chipset:pin)");

    if (parse_gpio(argv[0], &cfg->ctrl.id, &cfg->pin.id) < 0)
	USAGE_DIE("invalid GPIO pin (chipset:pin)");
}


/************************************************************************
 *                                                                      *
 ************************************************************************/

static void
reduced_lattency(void)
{
    // Change scheduler priority to be more "real-time"
    struct sched_param sp = {
        .sched_priority = sched_get_priority_max(SCHED_FIFO),
    };
    sched_setscheduler(0, SCHED_FIFO, &sp);

    // Avoid swapping by locking page in memory
    mlockall(MCL_CURRENT | MCL_FUTURE);
}


/************************************************************************
 *                                                                      *
 ************************************************************************/

static struct config config = {
    .ctrl.id   = "n/a",
    .ctrl.fd   = -1,
    .pin.id    = ~0,
    .pin.fd    = -1,
    .pin.flags = GPIO_V2_LINE_FLAG_EDGE_RISING,
    .pin.label = "pulse-counting",
};



int main(int argc, char *argv[])
{
    int                     rc      = -EINVAL;
    int                     fd      = -1;
    char                   *devpath = NULL;
    struct config          *cfg     = &config;

    // Configuration
    parse_config(argc, argv, cfg);
    
    // Build device path
    rc = asprintf(&devpath, "/dev/%s", cfg->ctrl.id);
    if (rc < 0) {
	errno = ENOMEM;
	LOG_ERRNO("unable to build path to device name");
	goto failed;
    }

    // Open device
    fd = open(devpath, O_RDONLY);
    if (fd < 0) {
	LOG_ERRNO("failed to open %s", devpath);
	goto failed;
    }
    LOG("controller device %s opened (fd=%d)", devpath, fd);

    // Get line (with a single gpio)
    struct gpio_v2_line_request req = {
	.num_lines        = 1,
	.offsets          = { [0] = cfg->pin.id },
	.config.flags     = GPIO_V2_LINE_FLAG_INPUT | cfg->pin.flags,
	.config.num_attrs = cfg->flags.debounce ? 1 : 0,
	.config.attrs     = {
	    { .mask                    = 1 << 0,
	      .attr.id                 = GPIO_V2_LINE_ATTR_ID_DEBOUNCE,
	      .attr.debounce_period_us = cfg->debounce                  }
	}
    };
    strncpy(req.consumer, cfg->pin.label, sizeof(req.consumer));
    
    // Release memory
    free(devpath);

    // Save controller file descriptor
    cfg->ctrl.fd = fd;

    // Call ioctl
    rc = ioctl(cfg->ctrl.fd, GPIO_V2_GET_LINE_IOCTL, &req);
    if (rc < 0) {
	LOG_ERRNO("failed to issue GPIO_V2_GET_LINE IOCTL for pin %d",
		  cfg->pin.id);
	return -errno;
    }
    
    // Store file descriptor
    cfg->pin.fd = req.fd;
    LOG("GPIO line configured as single pin %d (fd=%d)",
	cfg->pin.id, cfg->pin.fd);

    // Reduce latency
    reduced_lattency();

    PUT("started");
    
    while(1) {	
	if (cfg->flags.idle_timeout) {
	    struct timespec ts = {
		.tv_sec  = cfg->idle_timeout / 1000000,
		.tv_nsec = cfg->idle_timeout % 1000000 * 1000,
	    };
	    struct pollfd pfd = {
		.fd     = cfg->pin.fd,
		.events = POLLIN | POLLPRI
	    };
	    int rc = ppoll(&pfd, 1, &ts, NULL);
	    if (rc < 0) {
		LOG_ERRNO("ppoll failed");
		continue;
	    } else if (rc == 0) {
		PUT("idle");
		continue;
	    }
	}

	struct gpio_v2_line_event event[MAX_EVENTS];
	ssize_t size = read(cfg->pin.fd, event, sizeof(event));
	    
	if (size < 0) {
	    LOG_ERRNO("failed to read event");
	    continue;
	} else if (size % sizeof(struct gpio_v2_line_event)) {
	    LOG("got event of unexpected size");
	    continue;
	}

	struct timespec ts;
	clock_gettime(CLOCK_TAI, &ts);
	
	PUT("pulse -> %d", size / sizeof(struct gpio_v2_line_event));	
    }
    
    // Deal with failures
 failed:
    free(devpath);
    return errno;
}


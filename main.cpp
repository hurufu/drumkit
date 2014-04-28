//% Rollup drumkit userspace driver
//% Alex Grabovski <hurufu@gmail.com>
//% 2014-04-21
//% MIT License

#include <iostream>
#include <csignal>
#include <unistd.h>
#include <libusb.h>
#include <jack/jack.h>
#include <jack/midiport.h>

#define VERSION 0.3
#define DRUMKIT_VENDOR_ID 0x1941
#define DRUMKIT_PRODUCT_ID 0x8021
#define DRUMKIT_WMAXPACKETSIZE 8
#define DRUMKIT_ENDPOINT 1
#define NOF_PADS 6

// Error codes
#define SUCCESS 0
#define ERR_ANSWER_LENGTH_MISMATCH 0x1f
#define ERR_TRANSFER_FAILED 0x1e
#define ERR_LIBUSB_SPECIFIC 0x1d
#define ERR_JACK_SPECIFIC 0x1c
#define ERR_BAD_ARGUMENT 0x1b
#define ERR_UNKNOWN 0x1a
#define ERR_JACK_SERVER_CLOSED 0x19

// Program state flags
int r = SUCCESS; // Is used to temporary hold functions return values
int status = SUCCESS; // Overall program status
int verbatim = 0; // Verbosity level
bool run = true; // Main loop trigger

struct jack_callback_arg {
    jack_port_t * output_port;
    char pad_map[NOF_PADS];
    bool pad_states[NOF_PADS];
    int loop_buffer;
};

//TODO:
//
//  - dbus integration
//  - gui wrapper
//  - jack transport
//  - jackd autoquit
//  - ???
//  - PROFIT

libusb_device_handle * open_device(int vendor, int product);
static int process(jack_nframes_t nframes, void * arg);
void option_handler(int ac, char ** av, jack_callback_arg * drumkit_callback);
static void signal_handler(int);
void jack_shutdown_handler(void *);

int main (int ac, char ** av)
{
    libusb_device_handle * usb_drumkit_handle = NULL;
    jack_client_t * jack_midi_drum = NULL;
    const char jack_midi_drum_str[] = "Dreamlink Foldup Drumk Kit";
    volatile unsigned char raw[DRUMKIT_WMAXPACKETSIZE];
    int transfered;
    
    struct jack_callback_arg drumkit_callback;
    for (int i=0; i < NOF_PADS; i++)
        drumkit_callback.pad_map[i] = 0x20 + i;
    drumkit_callback.loop_buffer = 0;
    
    option_handler(ac, av, &drumkit_callback);
    
    // Initialisation sequence
    r = libusb_init(NULL);
    if (r != SUCCESS) {
        std::cerr << "ERROR: " << libusb_strerror((enum libusb_error)r) << "\n";
        return ERR_LIBUSB_SPECIFIC;
    }
    libusb_set_debug(NULL, LIBUSB_LOG_LEVEL_INFO);
    usb_drumkit_handle = open_device(DRUMKIT_VENDOR_ID, DRUMKIT_PRODUCT_ID);
    if ( usb_drumkit_handle == NULL ) {
        std::cerr << "ERROR: Unable to open drumkit! Probably it isn't connected.\n";
        status = ERR_LIBUSB_SPECIFIC;
        goto usb_exit;
    }
    r = libusb_set_auto_detach_kernel_driver(usb_drumkit_handle, 1);
    if (r != SUCCESS) {
        std::cerr << "ERROR: Could not set auto detachment of usb kernel driver. " << libusb_strerror((enum libusb_error)r) << "\n";
        status = ERR_LIBUSB_SPECIFIC;
        goto usb_close;
    }
    r = libusb_claim_interface(usb_drumkit_handle, 0);
    if (r != SUCCESS) {
        std::cerr << "ERROR: Cannot claim interface. " << libusb_strerror((enum libusb_error)r) << "\n";
        status = ERR_LIBUSB_SPECIFIC;
        goto usb_close;
    }
    jack_midi_drum = jack_client_open(jack_midi_drum_str, JackNullOption, NULL);
    if (jack_midi_drum == NULL) {
        std::cerr << "ERROR: Unable to initiate JACK client! Is JACK server running?\n";
        status = ERR_JACK_SPECIFIC;
        goto usb_release;
    }
    drumkit_callback.output_port = jack_port_register(jack_midi_drum, "out", JACK_DEFAULT_MIDI_TYPE, JackPortIsOutput, 0);
    jack_set_process_callback(jack_midi_drum, process, &drumkit_callback);
    jack_on_shutdown(jack_midi_drum, jack_shutdown_handler, &r);
    r = jack_activate(jack_midi_drum);
    if (r != SUCCESS) {
        std::cerr << "ERROR: Cannot activate JACK client --- " << r << "\n";
        status = ERR_JACK_SPECIFIC;
        goto jack_close;
    }
    
    signal(SIGQUIT, signal_handler);
    signal(SIGHUP,  signal_handler);
    signal(SIGTERM, signal_handler);
    signal(SIGINT,  signal_handler);
    
    // Main loop
    while(run) {
        unsigned char p = 0;
        for(int i = 0; i <= drumkit_callback.loop_buffer; i++)
        {
            r = libusb_interrupt_transfer (
                usb_drumkit_handle,
                DRUMKIT_ENDPOINT | LIBUSB_ENDPOINT_IN,
                (unsigned char*) raw,
                DRUMKIT_WMAXPACKETSIZE,
                &transfered,
                0
            );
            if(r >= 0) {
                if (transfered != DRUMKIT_WMAXPACKETSIZE) {
                    std::cerr << "ERROR: Answer length mismatch.\n";
                    status = ERR_ANSWER_LENGTH_MISMATCH;
                    goto jack_deact;
                }
                for (int j = 0; j < transfered; j++)
                    p |= raw[j];
            }
            else {
                std::cerr << "ERROR: " << libusb_strerror((enum libusb_error)r) << ".\n";
                status = ERR_TRANSFER_FAILED;
                goto jack_deact;
            }
        }
        for (int i = 0; i < NOF_PADS; i++) {
            drumkit_callback.pad_states[i] = p & 1;
            p >>= 1;
        }
    }

    // Gracefull exit
jack_deact:
    // It is impossible to deactivate jack client if server is not running
    if (status != ERR_JACK_SERVER_CLOSED)
        jack_deactivate(jack_midi_drum);
jack_close:
    // Same
    if (status != ERR_JACK_SERVER_CLOSED)
        jack_client_close(jack_midi_drum);
usb_release:
    libusb_release_interface(usb_drumkit_handle, 0);
usb_close:
    libusb_close(usb_drumkit_handle);
usb_exit:
    libusb_exit(NULL);
    return status;
}

void option_handler(int ac, char ** av, jack_callback_arg * drumkit_callback)
{
    //FIXME I dont have enough stamina to write getopt, and I don't want getopt
    // to be larger than actual driver! I have a plan 'bout ultimate™ getopt, 
    // but I don't think I'll make it soon...
    char c;
    int p_tmp = -1;
    int n_tmp;
    while ((c = getopt (ac, av, "Vhv:p:n:b:i:c:a")) != -1) {
        switch (c) {
            case 'V':
                std::cout << "Verison " << VERSION << std::endl;
                exit(0);
            case '?':
                r = ERR_BAD_ARGUMENT;
            case 'h':
                std::cout << "Usage:\n"
                    << "\t-h\t\tOutput this message and exit\n"
                    << "\t-v <level>\tVerbosity level (default 0). Values 1+ are not implemented\n"
                    << "\t-p <number>\tAsign to pad 0..5\n"
                    << "\t-n <note>\tnote in range 0..127\n"
                    << "\t-b <integer>\tSelect size of loop buffer (default 0)\n"
                    << "\t-i <integer>\tIntensity of note for selected pad [NOT IMPLEMENTED]\n"
                    << "\t-c <path>\tLoad config [NOT IMPLEMENTED\n]"
                    << "\t-a\t\tClose jackd on exit [NOT IMPLEMENTED]\n";
                exit(r);
            case 'v':
                verbatim = atoi(optarg);
                break;
            case 'p':
                p_tmp = atoi(optarg);
                if (p_tmp >= NOF_PADS) {
                    std::cerr << "ERROR: Bad pad number. Must be 0..5\n";
                    exit(ERR_BAD_ARGUMENT);
                }
                break;
            case 'n':
                n_tmp = atoi(optarg);
                if (n_tmp > 127) {
                    std::cerr << "ERROR: Bad note value. Must be 0..127\n";
                    exit(ERR_BAD_ARGUMENT);
                }
                drumkit_callback->pad_map[p_tmp] = n_tmp;
                break;
            case 'b':
                drumkit_callback->loop_buffer = atoi(optarg);
                break;
            default:
                abort();
        }
    }
    if (verbatim)
        std::cout << "INFO: Selected loop buffer size: " << drumkit_callback->loop_buffer << std::endl;
    
    if (p_tmp != -1) {
        std::cout << "INFO: Reassigned mapping:";
        for(int q=0; q<NOF_PADS; q++)
            std::cout << "\tPad" << q << ": " << (int) drumkit_callback->pad_map[q];
        std::cout << std::endl;
    }
}

static void signal_handler(int sig)
{
    run = false;
    std::cerr << "INFO: Signal " << sig << " received, exiting ...\n";
}

void jack_shutdown_handler(void * arg)
{
    //TODO: Generate signal SIGHUP or SIGUSR1.
    run = false;
    status = ERR_JACK_SERVER_CLOSED;
    std::cerr << "ERROR: JACK server is not running anymore, exiting\n";
}

static int process(jack_nframes_t nframes, void * arg)
{
    jack_callback_arg drumkit_callback = *(jack_callback_arg*) arg;
    void * port_buf = jack_port_get_buffer(drumkit_callback.output_port, nframes);
    unsigned char * midi_buffer;
    jack_midi_clear_buffer(port_buf);
    static bool was_free[6];
    
    for (int i = 0; i < 6; i++)
    if (drumkit_callback.pad_states[i]) {
        if (was_free[i]) {
            was_free[i] = false;
            midi_buffer = jack_midi_event_reserve(port_buf, 0, 3);
            midi_buffer[0] = 0x99;
            midi_buffer[1] = drumkit_callback.pad_map[i];
            midi_buffer[2] = 0x7F;
        }
    } else {
        if (!was_free[i]) {
            was_free[i] = true;
            midi_buffer = jack_midi_event_reserve(port_buf, 0, 3);
            midi_buffer[0] = 0x89;
            midi_buffer[1] = drumkit_callback.pad_map[i];
            midi_buffer[2] = 0x00;
        }
    }
    
    return 0;
}

libusb_device_handle * open_device(int vendor, int product)
{
    libusb_device ** devices;
    libusb_device_descriptor desc;
    libusb_config_descriptor * config;
    libusb_device_handle * devh;
    int r;

    int cnt = libusb_get_device_list(NULL, &devices);
    if (cnt < 0) {
        std::cerr << "ERROR: Cannot get usb devices list." << libusb_strerror((enum libusb_error)cnt) << "\n";
        return NULL;
    }

    for (int i = 0; i < cnt; i++) {
        libusb_get_device_descriptor(devices[i], &desc);
        if (desc.idVendor == vendor && desc.idProduct == product) {
            if (verbatim)
                std::cout << "INFO: Rollup Drumkit connected.\n" << std::endl;
            r = libusb_get_config_descriptor(devices[i], 0, &config);
            if (r < 0) {
                std::cerr << "ERROR: Cannot get config descriptor. " << libusb_strerror((enum libusb_error)r) << "\n";
                return NULL;
            }
            r = libusb_open(devices[i], &devh);
            if (r < 0) {
                std::cerr << "ERROR: Cannot open usb device. " << libusb_strerror((enum libusb_error)r) << "\n";
                return NULL;
            }
        }
    }
    libusb_free_device_list(devices, 1);
    return devh;
}





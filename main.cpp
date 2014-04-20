#include <iostream>
#include <csignal>
#include <unistd.h>

#include <libusb.h>
#include <jack/jack.h>
#include <jack/midiport.h>

#define DRUMKIT_VENDOR_ID 0x1941
#define DRUMKIT_PRODUCT_ID 0x8021

#define NOF_PADS 6

// Error codes
#define SUCCESS 0
#define ERR_ANSWER_LENGTH_MISMATCH 0xff
#define ERR_TRANSFER_FAILED 0xfe
#define ERR_LIBUSB_SPECIFIC 0xfd
#define ERR_JACK_SPECIFIC 0xfc
#define ERR_BAD_ARGUMENT 0xfb
#define ERR_UNKNOWN 0xfa

// Is it good to include such a dependency?
#include <boost/program_options.hpp>
namespace po = boost::program_options;

// Error handling variables
int r = SUCCESS;

libusb_device_handle * open_device(int vendor, int product);
static int process(jack_nframes_t nframes, void *arg);

static void sigexit(int);

libusb_device_handle * usb_drumkit_handle = NULL;
jack_client_t * jack_midi_drum = NULL;
jack_port_t * output_port;
volatile unsigned char buffer[8];

bool pad[6];
bool was_free[6];
bool verbatim = false;

int main (int ac, char* av[])
{
    //BEGIN getopt
    try {
        po::options_description usage("Usage");
        usage.add_options()
            ("help", "Output this message and exit.")
            ("verbatim", po::value<bool>(), "Show verbatim output.")
        ;
        po::variables_map vm;
        po::store(po::parse_command_line(ac, av, usage), vm);
        if (vm.count("help")) {
            std::cout << usage << std::endl;
            return 0;
        }
        if (vm.count("verbatim"))
            verbatim = true;
    } catch (std::exception & e) {
        std::cerr << "Error: " << e.what() << std::endl;
        return ERR_BAD_ARGUMENT;
    } catch (...) {
        std::cerr << "Uknown exception!" << std::endl;
        return ERR_UNKNOWN;
    }
    //END getopt
    
    
    int transfered;
    jack_nframes_t nframes;
    const char jack_midi_drum_str[] = "Dreamlink Foldup Drumk Kit";

    r = libusb_init(NULL);
    if (r != SUCCESS) {
        std::cerr << "libusb_init: " << libusb_strerror((enum libusb_error)r) << std::endl;
        return ERR_LIBUSB_SPECIFIC;
    }
    libusb_set_debug(NULL, LIBUSB_LOG_LEVEL_INFO);
    usb_drumkit_handle = open_device(DRUMKIT_VENDOR_ID, DRUMKIT_PRODUCT_ID);
    if ( usb_drumkit_handle == NULL ) {
        std::cerr << "Unable to open drumkit! Probably it isn't connected." << std::endl;
        libusb_exit(NULL);
        return ERR_LIBUSB_SPECIFIC;
    }
    r = libusb_set_auto_detach_kernel_driver(usb_drumkit_handle, 1);
    if (r != SUCCESS) {
        std::cerr << "libusb_set_auto_detach_kernel_driver: " << libusb_strerror((enum libusb_error)r) << std::endl;
        libusb_close(usb_drumkit_handle);
        libusb_exit(NULL);
        return ERR_LIBUSB_SPECIFIC;
    }
    r = libusb_claim_interface(usb_drumkit_handle, 0);
    if (r != SUCCESS) {
        std::cerr << "libusb_claim_interface: " << libusb_strerror((enum libusb_error)r) << std::endl;
        libusb_close(usb_drumkit_handle);
        libusb_exit(NULL);
        return ERR_LIBUSB_SPECIFIC;
    }

    jack_midi_drum = jack_client_open(jack_midi_drum_str, JackNullOption, NULL);
    if (jack_midi_drum == NULL) {
        std::cerr << "Unable to initiate jack client! Is JACK server running?" << std::endl;
        libusb_release_interface(usb_drumkit_handle, 0); // Check exit codes
        libusb_close(usb_drumkit_handle);
        libusb_exit(NULL);
        return ERR_JACK_SPECIFIC;
    }

    jack_set_process_callback(jack_midi_drum, process, 0);

    output_port = jack_port_register (jack_midi_drum, "out", JACK_DEFAULT_MIDI_TYPE, JackPortIsOutput, 0);
    nframes = jack_get_buffer_size(jack_midi_drum);

    r = jack_activate(jack_midi_drum);
    if (r != SUCCESS) {
        std::cerr << "Cannot activate client --- " << r << std::endl;
        jack_client_close(jack_midi_drum);
        libusb_close(usb_drumkit_handle);
        libusb_exit(NULL);
        return ERR_JACK_SPECIFIC;
    }
    
    signal(SIGQUIT, sigexit);
    signal(SIGHUP,  sigexit);
    signal(SIGTERM, sigexit);
    signal(SIGINT,  sigexit);
        
    while(true)
    {
        r = libusb_interrupt_transfer(usb_drumkit_handle, 0x81, (unsigned char*) buffer, sizeof(buffer), &transfered, 0);
        if(r >= 0) {
            if (transfered != sizeof(buffer)) {
                std::cerr << "Answer length mismatch." << std::endl;
                r = ERR_ANSWER_LENGTH_MISMATCH;
                break;
            }
            for (int i = 0; i < NOF_PADS; i++) {
                pad[i] = buffer[0] & 1;
                buffer[0] >>= 1;
            }
        }
        else {
            std::cerr << libusb_strerror((enum libusb_error)r) << "." << std::endl;
            r = ERR_TRANSFER_FAILED;
            break;
        }
    }

    jack_deactivate(jack_midi_drum);
    jack_client_close(jack_midi_drum);
    libusb_release_interface(usb_drumkit_handle, 0);
    libusb_close(usb_drumkit_handle);
    libusb_exit(NULL);

    return r;
}

static void sigexit(int sig)
{
    jack_deactivate(jack_midi_drum);
    jack_client_close(jack_midi_drum);
    libusb_release_interface(usb_drumkit_handle, 0);
    libusb_close(usb_drumkit_handle);
    libusb_exit(NULL);

    std::cerr << "Signal " << sig << " received, exiting ..." << std::endl;
    exit(0);
}

static int process(jack_nframes_t nframes, void *arg)
{
    void * port_buf = jack_port_get_buffer(output_port, nframes);
    unsigned char * midi_buffer;
    jack_midi_clear_buffer(port_buf);
    
    for (int i = 0; i < 6; i++)
    if (pad[i]) {
        if (was_free[i]) {
            was_free[i] = false;
            midi_buffer = jack_midi_event_reserve(port_buf, 0, 3);
            midi_buffer[0] = 0x9A;
            midi_buffer[1] = 0x20 + i;
            midi_buffer[2] = 0x7F;
        }
    } else {
        if (!was_free[i]) {
            was_free[i] = true;
            midi_buffer = jack_midi_event_reserve(port_buf, 0, 3);
            midi_buffer[0] = 0x8A;
            midi_buffer[1] = 0x20 + i;
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
    libusb_device_handle * usb_drumkit_handle;
    int r;

    int cnt = libusb_get_device_list(NULL, &devices);
    if (cnt < 0) {
        std::cerr << "libusb_get_device_list: " << libusb_strerror((enum libusb_error)cnt) << std::endl;
        return NULL;
    }

    for (int i = 0; i < cnt; i++) {
        libusb_get_device_descriptor(devices[i], &desc);
        if (desc.idVendor == vendor && desc.idProduct == product) {
            if (verbatim)
                std::cout << "Rollup Drumkit connected!\n";
            
            r = libusb_get_config_descriptor(devices[i], 0, &config);
            if (r < 0) {
                std::cerr << "libusb_get_config_descriptor: " << libusb_strerror((enum libusb_error)r) << std::endl;
                return NULL;
            }
            
            r = libusb_open(devices[i], &usb_drumkit_handle);
            if (r < 0) {
                std::cerr << "libusb_open: " << libusb_strerror((enum libusb_error)r) << std::endl;
                return NULL;
            }
        }
    }
    libusb_free_device_list(devices, 1);
    return usb_drumkit_handle;
}





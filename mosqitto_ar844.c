// MQTT publisher of AR844 smart sensor sound level meter
// Uses mosquitto mqtt library, and libusb1.0
//
// Configuration values are in the code.
//
// published format tele/*hostname*/ar844/data
//   { "time": "2019-12-29T13:45:00Z",
//     "min": 13.2,
//     "max": 72.4,
//     "avg": 50.0,
//     "weight": "A"
//   }
//
// Based on...
//  libusb test code - https://www.microchip.com/forums/m340898.aspx
//  meter reverse engineering - http://www.brainworks.it/rpi-environmental-monitoring/reveng-the-usb-data
//
#include <errno.h> 
#include <string.h> 
#include <stdio.h> 
#include <stdlib.h> 
#include <unistd.h> 
#include <time.h> 
#include <signal.h> 
#include <libusb-1.0/libusb.h> 
#include <mosquitto.h>

// Config
const int   meter_poll_period = 500;                // ms - poll meter every 500ms
const int   meter_accumulation_period = 1*60;       // s  - accumulate readings and publish every 60 seconds
const char *mqqt_broker_hostname = "server";        // broker host name
const int   mqqt_broker_port = 1883;                // broker port
const char *mqqt_topic = "tele/%s/ar844/data";      // publish topic - %s will contain hostname
#define     MQQT_KEEPALIVE  90                      // connection keep alive time (seconds)

// AR844 VID and PID (dodgey?)
#define VENDOR_ID 0x1234
#define PRODUCT_ID 0x5678
// endpoint information (we could query this)
#define PACKET_INT_OUT_LEN     8 
#define PACKET_INT_IN_LEN     8
const static int ENDPOINT_INT_IN=0x81; /* endpoint 0x81 address for IN */ 
const static int ENDPOINT_INT_OUT=0x02; /* endpoint 1 address for OUT */ 
const static int TIMEOUT=1000; /* timeout in ms */  

static libusb_context *usbCtx;
static struct libusb_device_handle *devh = NULL;  
static struct mosquitto *mqqt_client = NULL;

volatile int doExit = 0;
static char hostname[256];

void signal_handler(int n)
{
    fprintf(stderr, "SIGINT received\n");
    doExit = 1;
}

int timespec_subtract (struct timespec *x, struct timespec *y)
{
    int sec_diff = (x->tv_sec - y->tv_sec) * 1000;	// s to ms
    int ns_diff = (x->tv_nsec - y->tv_nsec) / 1000000;	// us to ms
    return sec_diff + ns_diff;
}

static int init_mqqt()
{
    int r;

    r = mosquitto_lib_init();

    mqqt_client = mosquitto_new(NULL, true, NULL );
    if ( mqqt_client == NULL )
    {
        fprintf(stderr, "Failed to create mosquitto client %d", errno );
        return -errno;
    }

    r = mosquitto_connect( mqqt_client, mqqt_broker_hostname, mqqt_broker_port, MQQT_KEEPALIVE );
    if ( r != MOSQ_ERR_SUCCESS )
    {
        fprintf(stderr, "Failed to connect to host %s:%d %d", mqqt_broker_hostname, mqqt_broker_port, r );
        return -r;
    }

    return 0;
}

static void publish_sample(const char * msg )
{
    int r;

    char topic[200];
    snprintf( topic, sizeof(topic), mqqt_topic, hostname );

    for ( int i = 0; i < 2; i++ )
    {
        r = mosquitto_publish( mqqt_client, NULL, topic, strlen(msg), msg, 0, false );
        if ( r == MOSQ_ERR_SUCCESS )
            break;
        if ( r == MOSQ_ERR_NO_CONN )
            mosquitto_reconnect( mqqt_client );
    }
}


uint32_t dBSum = 0;
uint16_t dBMin = 0;
uint16_t dBMax = 0;
int sampleCount = 0;
time_t next_period;

static void get_next_period()
{
    next_period = ((time(NULL) + meter_accumulation_period) / meter_accumulation_period ) * meter_accumulation_period;
    //fprintf(stderr, "Next period = %d\n", next_period );
}

static void process_sample( uint16_t dB, bool fast, char weight, int range )
{
    if ( sampleCount == 0 )
    {
        dBMin = dBMax = dBSum = dB;
    }
    else
    {
        dBSum += dB;
        if ( dB < dBMin )
            dBMin = dB;
        else if ( dB > dBMax )
            dBMax = dB;
    }
    sampleCount++;

    if ( time(NULL) >= next_period )
    {
        uint32_t dBAvg = dBSum / sampleCount;
        char timebuf[30], buf[200];
        strftime(timebuf, sizeof(timebuf), "%FT%TZ", localtime(&next_period) );
        snprintf(buf, sizeof(buf),
                        "{\"time\": \"%s\","
                        "\"avg\": %d.%d,"
                        "\"min\": %d.%d,"
                        "\"max\": %d.%d,"
                        "\"weight\": \"%c\""
                        "}", 
                        timebuf, 
                        dBAvg/10,dBAvg%10, 
                        dBMin/10,dBMin%10, 
                        dBMax/10,dBMax%10, 
                        weight );
        fprintf(stderr, "%s\n", buf);
        publish_sample(buf);
        get_next_period();
        sampleCount = 0;
    }
}

static int main_loop(void) 
{ 
    int r,i; 
    int transferred; 
    uint8_t answer[PACKET_INT_IN_LEN] = {0}; 
    uint8_t question[PACKET_INT_OUT_LEN] = {0xB3,0x50,0x05,0x16,0x24,0x11,0x19,0x00};   // I don't think the content of the poll packet matters.

    // set up a send and receive async transfer to run simultaneously.
    struct libusb_transfer *send = libusb_alloc_transfer(0);
    struct libusb_transfer *recv = libusb_alloc_transfer(0);

    libusb_fill_interrupt_transfer( send, devh, ENDPOINT_INT_OUT, question, PACKET_INT_OUT_LEN, NULL, NULL, 1000);
    libusb_fill_interrupt_transfer( recv, devh, ENDPOINT_INT_IN, answer, PACKET_INT_IN_LEN, NULL, NULL, 1000);
    recv->flags = LIBUSB_TRANSFER_SHORT_NOT_OK;

    recv->status = -1;
    r = libusb_submit_transfer( recv );
    //fprintf(stderr,"recv status=%d\n", recv->status );
    if ( r < 0 )
    {
        fprintf(stderr, "Failed to submit recv transfer %d\n", r );
        return 0;
    }
    send->status = -1;
    r = libusb_submit_transfer( send );
    //fprintf(stderr, "send status=%d\n", send->status );
    if ( r < 0 )
    {
        fprintf(stderr, "Failed to submit send transfer %d\n", r );
        return 0;
    }
    struct timespec last;
    clock_gettime( CLOCK_REALTIME, &last );
    int count = 0;
    while ( !doExit )
    {
    //    fprintf(stderr, "handle events\n");
        struct timeval tv;
        tv.tv_sec = 1;
        tv.tv_usec = 0;
        int completed = 0;
        libusb_handle_events_timeout_completed( usbCtx, &tv, &completed );
     //   fprintf(stderr,"completed=%d\n", completed );


      //  fprintf(stderr,"recv status=%d\n", recv->status );
        if ( recv->status >= 0 )
        {
            if ( recv->status == LIBUSB_TRANSFER_COMPLETED )
            {
                //if ( recv->actual_length > 0 ) 
                //{
                //     for(i = 0;i < recv->actual_length; i++) { 
                //         if(i%8 == 0) 
                //             printf("\n"); 
                //         printf("%02x ",(uint8_t)answer[i]); 
                //     } 
                //     printf("\n"); 
                //}

                if ( recv->actual_length == PACKET_INT_IN_LEN ) 
                {
                     uint16_t dB = (((uint16_t)answer[0]) << 8) | answer[1];
                     float soundLeveldB = (float)dB/10.0;
                     int measureSpeed = answer[2] >> 6;
                     int measureCurveType = (answer[2] >> 4) & 0x01;
                     int measureRange = answer[2] & 0x07;
                    
                     //printf("%f %s %s %d\n", soundLeveldB, measureSpeed==1?"FAST":"SLOW",measureCurveType==0?"A":"C",measureRange);
                     process_sample( dB, measureSpeed, measureCurveType==0?'A':'Z', measureRange );
                }
            }
            else
            {
                //fprintf(stderr, "recv status error %d\n", recv->status );
            }
            recv->status = -1;
            r = libusb_submit_transfer( recv );
            //fprintf(stderr,"recv status=%d\n", recv->status );
        }
        
        //fprintf(stderr, "send status=%d\n", send->status );
        if ( send->status >= 0 )
        {
			struct timespec now;
			clock_gettime( CLOCK_REALTIME, &now );
			int diff = timespec_subtract(&now, &last );

            if ( diff >= 500 )
			{
                last = now;
                send->status = -1;
                r = libusb_submit_transfer( send );
                //fprintf(stderr, "send status=%d\n", send->status );
			}
        }
    }
    return 0; 
 } 

static int init_usb()
{
    int r;
    
    devh = NULL;
    r = libusb_init(&usbCtx); 
    if (r < 0) 
    { 
        fprintf(stderr, "Failed to initialise libusb\n"); 
        return r;
    } 

    //libusb_set_option(usbCtx, LIBUSB_OPTION_LOG_LEVEL , LIBUSB_LOG_LEVEL_DEBUG );
   
    devh = libusb_open_device_with_vid_pid(NULL, VENDOR_ID, PRODUCT_ID); 
    if ( devh == NULL )
    { 
        r = -EIO; 
        fprintf(stderr, "Could not find/open Smart Sensor AR844\n"); 
        goto out; 
    } 
    //printf("Successfully find the LVR Generic HID device\n"); 

    libusb_set_auto_detach_kernel_driver(devh, 1);
 #ifdef LINUX 
    libusb_detach_kernel_driver(devh, 0);      
 #endif 
 
    r = libusb_set_configuration(devh, 1); 
    if (r < 0) 
    { 
        fprintf(stderr, "libusb_set_configuration error %d\n", r); 
        //goto out; 
    } 
    //printf("Successfully set usb configuration 1\n"); 
    //
    r = libusb_claim_interface(devh, 0); 
    if (r < 0) 
    { 
        fprintf(stderr, "libusb_claim_interface error %d\n", r); 
        goto out; 
    } 
    //printf("Successfully claimed interface\n"); 
    return 0;
out:
    if ( devh != NULL )
    {
        libusb_release_interface(devh, 0); 
        libusb_reset_device(devh); 
        libusb_close(devh); 
    }
    libusb_exit(NULL); 
    return r;
}
   
int main(void) 
{ 
    int r = 1; 

    signal(SIGINT, signal_handler);

    r = init_usb();
    if ( r < 0 )
    {
        fprintf(stderr, "Failed to initialise usb\n"); 
        exit(1); 
    }
    
    r = init_mqqt();
    if ( r < 0 )
    {
        fprintf(stderr, "Failed to initialise mqtt\n"); 
        goto out;
    }

    get_next_period();
    gethostname( hostname, sizeof(hostname) );
    main_loop(); 
 
out: 
    libusb_release_interface(devh, 0); 
    libusb_reset_device(devh); 
    libusb_close(devh); 
    libusb_exit(NULL); 

    mosquitto_disconnect(mqqt_client);
    mosquitto_destroy(mqqt_client);
    mosquitto_lib_cleanup();
    return r >= 0 ? r : -r;  
 } 
 
 

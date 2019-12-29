
#include <errno.h> 
#include <string.h> 
#include <stdio.h> 
#include <stdlib.h> 
#include <unistd.h> 
#include <libusb-1.0/libusb.h> 

#define VERSION "0.1.0"  
#define VENDOR_ID 0x1234
#define PRODUCT_ID 0x5678
 
 // HID Class-Specific Requests values. See section 7.2 of the HID specifications 
 #define HID_GET_REPORT                0x01 
 #define HID_GET_IDLE                  0x02 
 #define HID_GET_PROTOCOL              0x03 
 #define HID_SET_REPORT                0x09 
 #define HID_SET_IDLE                  0x0A 
 #define HID_SET_PROTOCOL              0x0B 
 #define HID_REPORT_TYPE_INPUT         0x01 
 #define HID_REPORT_TYPE_OUTPUT        0x02 
 #define HID_REPORT_TYPE_FEATURE       0x03 
   
 #define CTRL_IN        LIBUSB_ENDPOINT_IN|LIBUSB_REQUEST_TYPE_CLASS|LIBUSB_RECIPIENT_INTERFACE 
 #define CTRL_OUT    LIBUSB_ENDPOINT_OUT|LIBUSB_REQUEST_TYPE_CLASS|LIBUSB_RECIPIENT_INTERFACE 
 
 
 const static int PACKET_CTRL_LEN=8;   
 #define PACKET_INT_OUT_LEN     8 
 #define PACKET_INT_IN_LEN     8
 const static int INTERFACE=0; 
 const static int ENDPOINT_INT_IN=0x81; /* endpoint 0x81 address for IN */ 
 const static int ENDPOINT_INT_OUT=0x02; /* endpoint 1 address for OUT */ 
 const static int TIMEOUT=1000; /* timeout in ms */  

libusb_context *usbCtx;

volatile int doExit = 0;

 void bad(const char *why) { 
     fprintf(stderr,"Fatal error> %s\n",why); 
     exit(17); 
 } 
   
 static struct libusb_device_handle *devh = NULL;  
   
 static int find_lvr_hidusb(void) 
 { 
     devh = libusb_open_device_with_vid_pid(NULL, VENDOR_ID, PRODUCT_ID); 
     return devh ? 0 : -EIO; 
 }  
 
 
static int test_interrupt_transfer(void) 
{ 
    int r,i; 
    int transferred; 
    uint8_t answer[PACKET_INT_IN_LEN] = {0}; 
    uint8_t question[PACKET_INT_OUT_LEN] = {0xB3,0x50,0x05,0x16,0x24,0x11,0x19,0x00};

    int count = 0;
    while ( !doExit )
    {
         fprintf(stderr, "Interrupt write %d\n", count++ );
        r = libusb_interrupt_transfer(devh, ENDPOINT_INT_OUT, question, PACKET_INT_OUT_LEN, &transferred,TIMEOUT); 
        if (r < 0) { 
             fprintf(stderr, "Interrupt write error %d\n", r); 
             goto error;
         } 
         fprintf(stderr, "r=%d, transferred=%d\n", r, transferred );

         fprintf(stderr, "Interrupt read\n" );
         transferred = 0;
         r = libusb_interrupt_transfer(devh, ENDPOINT_INT_IN, answer,PACKET_INT_IN_LEN, 
             &transferred, TIMEOUT); 
         fprintf(stderr, "r=%d, transferred=%d\n", r, transferred );
         if (r < 0) { 
             fprintf(stderr, "Interrupt read error %d\n", r); 
             goto error;
         } 
         if (transferred < PACKET_INT_IN_LEN) { 
             fprintf(stderr, "Interrupt transfer short read (%d)\n", r); 
             //return -1; 
         } 
     
         for(i = 0;i < transferred; i++) { 
             if(i%8 == 0) 
                 printf("\n"); 
             printf("%02x ",(uint8_t)answer[i]); 
         } 
         printf("\n"); 
    
         if ( transferred == 8 )
         {
             uint16_t dB = (((uint16_t)answer[0]) << 8) | answer[1];
             float soundLeveldB = (float)dB/10.0;
             int measureSpeed = answer[2] >> 6;
             int measureCurveType = (answer[2] >> 4) & 0x01;
             int measureRange = answer[2] & 0x07;
            
             printf("%f %s %s %d\n", soundLeveldB, measureSpeed==1?"FAST":"SLOW",measureCurveType==0?"A":"C",measureRange);
         }

         sleep(1);
         continue;
      error:
         printf("Reseting device\n"); 
         libusb_reset_device( devh );
    }
return 0; 
 } 
   
 int main(void) 
 { 
     int r = 1; 
 
     r = libusb_init(&usbCtx); 
     if (r < 0) { 
         fprintf(stderr, "Failed to initialise libusb\n"); 
         exit(1); 
     } 

    libusb_set_option(usbCtx, LIBUSB_OPTION_LOG_LEVEL , LIBUSB_LOG_LEVEL_DEBUG );

     r = find_lvr_hidusb(); 
     if (r < 0) { 
         fprintf(stderr, "Could not find/open LVR Generic HID device\n"); 
         goto out; 
     } 
     printf("Successfully find the LVR Generic HID device\n"); 

    libusb_set_auto_detach_kernel_driver(devh, 1);
 #ifdef LINUX 
      libusb_detach_kernel_driver(devh, 0);      
 #endif 
 
     r = libusb_set_configuration(devh, 1); 
     if (r < 0) { 
         fprintf(stderr, "libusb_set_configuration error %d\n", r); 
     //    goto out; 
     } 
     printf("Successfully set usb configuration 1\n"); 
     r = libusb_claim_interface(devh, 0); 
     if (r < 0) { 
         fprintf(stderr, "libusb_claim_interface error %d\n", r); 
         goto out; 
     } 
     printf("Successfully claimed interface\n"); 
 
     //printf("Testing control transfer using loop back test of feature report"); 
     //test_control_transfer(); 
 
     //printf("Testing control transfer using loop back test of input/output report"); 
     //test_control_transfer_in_out(); 
      
     printf("Testing interrupt transfer using loop back test of input/output report"); 
     test_interrupt_transfer(); 
 
     printf("\n"); 
   
     libusb_release_interface(devh, 0); 
 out: 
 //    libusb_reset_device(devh); 
     libusb_close(devh); 
     libusb_exit(NULL); 
     return r >= 0 ? r : -r;  
 } 
 
 

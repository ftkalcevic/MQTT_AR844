# MQTT publisher for AR844 Smart Sensor Sound Level Meter

This is linux code to read the AR844 Smart Sensor sound level meter.

I polls the usb device (yes polls) every 500ms.  It accumulates the results for one minute then publishes the avg/min/max
to the MQTT subject tele/HOSTNAME/ar844/data.

It uses mosquitto, and libusb-1.0 libraries.

I use this with a Pi Zero W to remotely monitor noise.


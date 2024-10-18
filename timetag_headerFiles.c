

/* declaration of functions in timetag_io.c */
int initialize_DAC(int handle);
int initialize_DAC(int handle);
int set_DAC_channel(int handle, int channel, int value);
int initialize_rfsource(int handle);
int rfsource_internal_reference(int handle);
int rfsource_external_reference(int handle);
int _rfsource_set_registers(int handle, int t, int n, int m);
void usb_flushmode(int handle, int mode);
int adjust_rfsource(int handle, int ftarget, int fref);
void set_inhibit_line(int handle, int state);
void set_calibration_line(int handle, int state);
void initialize_FIFO(int handle);
void fifo_partial_reset(int handle);
void start_dma(int handle);
void stop_dma(int handle);
void reset_slow_counter(int handle);
void Reset_gadget(int handle);



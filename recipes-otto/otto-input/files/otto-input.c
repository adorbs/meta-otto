/*  File: otto-input.c
	Description: I2C input to FIFO program for TOOT
	Author: Steven Hang <github.com/adorbs>
*/

#include <stdio.h>
#include <math.h>
#include <stdlib.h>
#include <fcntl.h>
#include <unistd.h>
#include <string.h>
#include <errno.h>
#include <signal.h>
#include <assert.h>
#include <sys/ioctl.h>
#include <linux/i2c.h>
#include <linux/i2c-dev.h>
#include <linux/input.h>
#include <linux/uinput.h>
#include <sys/types.h>
#include <sys/stat.h>

#define NUM_ROWS		8
#define NUM_COLUMNS		7
#define NUM_ENCODERS	4
#define REPORT_BYTES	NUM_ROWS+NUM_ENCODERS

#define I2C_SLAVE_ADDR 	0x77

static int is_signaled = 0;	/* Exit program if signaled */
static int i2c_fd = -1;		/* Open /dev/i2c-1 device */
static int f_debug = 0;		/* True to print debug messages */
static int fifo_fd = -1;	/* fifo file descriptor */

typedef struct input_data_t{
	unsigned char ROW_1;
	unsigned char ROW_2;
	unsigned char ROW_3;
	unsigned char ROW_4;
	unsigned char ROW_5;
	unsigned char ROW_6;
	unsigned char ROW_7;
	unsigned char ROW_8;
	unsigned char ENCODER_BLUE; // Blue
	unsigned char ENCODER_GREEN; // Green
	unsigned char ENCODER_YELLOW; // Yellow
	unsigned char ENCODER_RED; // Red
}input_data_t;

const char * const keyNames[NUM_ROWS][NUM_COLUMNS] = 
	{{"C1",  "C4",  "C8",    "SHIFT",   "SENDS",    "C6:R1",   "BLUE"},
	 {"S2",  "S7", "S12",     "OCT+", "ROUTING",   "RECORD", "YELLOW"},
	 {"S3",  "S8", "S13",     "OCT-",     "FX2",   "MASTER",  "R3:C7"},
	 {"C2",  "C5",  "C9",       "S1",     "FX1",     "PLAY",    "RED"},
	 {"S4",  "S9", "S14",      "S16",     "ARP",	"SLOTS",  "R5:C7"},
	 {"S5", "S10", "S15",   "TWIST2",  "LOOPER",   "TWIST1",  "R6:C7"},
	 {"C3",  "C6", "C10",      "EXT", "SAMPLER", "ENVELOPE",  "GREEN"},
	 {"S6",  "C7", "S11", "SETTINGS",     "SEQ",    "SYNTH",  "R8:C7"}};

const char const keyCodes[NUM_ROWS][NUM_COLUMNS] = 
	{{(2<<6)|0, (2<<6)|3, (2<<6)| 7, (1<<6)|17, (1<<6)| 3,        -1, (0<<6)| 0},
	 {(3<<6)|1, (3<<6)|6, (3<<6)|11, (1<<6)| 1, (1<<6)| 4, (1<<6)| 5, (0<<6)| 2},
	 {(3<<6)|2, (3<<6)|7, (3<<6)|12, (1<<6)| 2, (1<<6)| 7, (1<<6)| 0,        -1},
	 {(2<<6)|1, (2<<6)|4, (2<<6)| 8, (3<<6)| 0, (1<<6)| 6, (1<<6)| 8, (0<<6)| 3},
	 {(3<<6)|3, (3<<6)|8, (3<<6)|13, (3<<6)|15, (1<<6)|13, (1<<6)|18,        -1},
	 {(3<<6)|4, (3<<6)|9, (3<<6)|14, (1<<6)|20, (1<<6)| 9, (1<<6)|19,        -1},
	 {(2<<6)|2, (2<<6)|5, (2<<6)| 9, (1<<6)|16, (1<<6)|11, (1<<6)|15, (0<<6)| 1},
	 {(3<<6)|5, (2<<6)|6, (3<<6)|10, (1<<6)|12, (1<<6)|10, (1<<6)|14,        -1}};

const char * const encoderNames[NUM_ENCODERS] =
	{"BLUE", "GREEN", "YELLOW", "RED"};

const char const encoderCodes[NUM_ENCODERS] =
	{0x30, 0x31, 0x32, 0x33};

static void timed_wait(long sec,long usec,long early_usec) 
{
    fd_set mt;
    struct timeval timeout;
    int rc;

    FD_ZERO(&mt);
    timeout.tv_sec = sec;
    timeout.tv_usec = usec;
    do  {
        rc = select(0,&mt,&mt,&mt,&timeout);
        if ( !timeout.tv_sec && timeout.tv_usec < early_usec )
            return;     /* Wait is good enough, exit */
    } while ( rc < 0 && timeout.tv_sec && timeout.tv_usec );
}

static void copy_input_data(input_data_t* now, input_data_t* last)
{
	// Copy current data over to last
	memcpy(last, now, sizeof(input_data_t)/sizeof(char));
}

	
/*
 * Open I2C bus and check capabilities :
 */
static void i2c_init(const char *node) {
	unsigned long i2c_funcs = 0;	/* Support flags */
	int rc;

	i2c_fd = open(node,O_RDWR);	/* Open driver /dev/i2s-1 */
	if ( i2c_fd < 0 ) {
		perror("Opening /dev/i2c-1");
		puts("Check that the i2c-dev & i2c-bcm2708 kernel modules "
		     "are loaded.");
		abort();
	}

	/*
	 * Make sure the driver supports plain I2C I/O:
	 */
	rc = ioctl(i2c_fd,I2C_FUNCS,&i2c_funcs);
	assert(rc >= 0);
	assert(i2c_funcs & I2C_FUNC_I2C);
}

/*
 * Read data:
 */
static int input_read(input_data_t* data)
{
	struct i2c_rdwr_ioctl_data msgset;
	struct i2c_msg iomsgs[1];
	char zero[1] = { 0x00 };	/* Written byte */
	int rc;

	/*
	 * Send 0x00 command
	 */
	iomsgs[0].addr = I2C_SLAVE_ADDR; /* i2c address */
	iomsgs[0].flags = 0;			 /* Write */
	iomsgs[0].buf = zero;			 /* Transmit buf */
	iomsgs[0].len = 1;				 /*  bytes */

	msgset.msgs = iomsgs;
	msgset.nmsgs = 1;

	rc = ioctl(i2c_fd,I2C_RDWR,&msgset);
	if ( rc < 0 )
		return -1;		/* I/O error */

	timed_wait(0,200,0);		/* Give MCU Time */

	/*
	 * Read 12 bytes
	 */
	iomsgs[0].addr = I2C_SLAVE_ADDR; /* i2c address */
	iomsgs[0].flags = I2C_M_RD;		 /* Read */
	iomsgs[0].buf = (char *)data;	 /* Receive buf */
	iomsgs[0].len = REPORT_BYTES;	 /* 12 bytes */

	msgset.msgs = iomsgs;
	msgset.nmsgs = 1;

	rc = ioctl(i2c_fd,I2C_RDWR,&msgset);
	if ( rc < 0 )
		return -1;			/* Failed */
	
	return 0;
}

/*
 * Close the I2C driver :
 */
static void i2c_close(void)
{
	close(i2c_fd);
	i2c_fd = -1;
}

/*
 * Signal handler to quit the program :
 */
static void sigint_handler(int signo)
{
	is_signaled = 1;		/* Signal to exit program */
}

//Checks two input states to see if there was a change
static int inputChange(input_data_t* now, input_data_t* last)
{
	if ( memcmp(now, last, sizeof(input_data_t) ) == 0 )
	{
		return 0; // No change
	}
	else
	{
		return 1; // There was a change
	}
}

/*
 * Generate the report of the input changes.
 * This can be made MUCH more efficient
 */
static int generateReport(input_data_t* now, input_data_t* last, char* buffer, char* debugBuffer)
{
	int i, j;
	unsigned char diff, mask, nowChar, lastChar;
	int numBytes = 0;
	debugBuffer[0] = '\0'; // Make first char null terminator so strcat can work

	for (i=0; i<NUM_ROWS; i++)
	{
		nowChar = ((unsigned char *)now)[i];
		lastChar = ((unsigned char *)last)[i];
		diff = nowChar ^ lastChar;
		for (j=0; j<NUM_COLUMNS; j++)
		{
			mask = 0x01 << j;
			if (diff & mask)
			{
				if ( (nowChar & mask) > (lastChar & mask) )
				{
					buffer[numBytes++] = 0x20;
					strcat(debugBuffer, "DOWN: ");
				}
				else
				{
					buffer[numBytes++] = 0x21;
					strcat(debugBuffer, "UP: ");
				}
				buffer[numBytes++] = keyCodes[i][j];
				buffer[numBytes++] = '\n';
				strcat(debugBuffer, keyNames[i][j]);
				strcat(debugBuffer,"\n");
			}
		}
	}
	for (i=NUM_ROWS; i<NUM_ROWS+NUM_ENCODERS; i++)
	{
		nowChar = ((unsigned char *)now)[i];
		lastChar = ((unsigned char *)last)[i];
		diff = nowChar ^ lastChar;
		if (diff)
		{
			buffer[numBytes++] = encoderCodes[i-NUM_ROWS];
			strcat(debugBuffer, encoderNames[i-NUM_ROWS]);
			if (nowChar > lastChar)
			{
				if ((nowChar - lastChar) > 64)
				{
					buffer[numBytes++] = -1;
					strcat(debugBuffer,"-");
				}
				else
				{
					buffer[numBytes++] = 1;
					strcat(debugBuffer,"+");
				}
			}
			else
			{
				if ((lastChar - nowChar) > 64)
				{
					buffer[numBytes++] = 1;
					strcat(debugBuffer,"+");
				}
				else
				{
					buffer[numBytes++] = -1;
					strcat(debugBuffer,"-");
				}
			}
			buffer[numBytes++] = '\n';
			strcat(debugBuffer,"\n");
		}
	}

	return numBytes;
}

static void fifo_init(const char *node) 
{
	int status;

	status = mkfifo(node, 0666);

	if ( status < 0 ) {
		perror("Error making FIFO");
		abort();
	}

	fifo_fd = open(node, O_RDWR);

	if ( fifo_fd < 0 ) {
		perror("Error opening FIFO");
		abort();
	}
}

static void fifo_close(const char *node)
{
	close(fifo_fd);
	fifo_fd = -1;
}
/*
 * Main program :
 */
void main(int argc,char **argv)
{
	input_data_t inputState, prevState;
	char debugBuffer[256];
	char reportBuffer[256];
	int reportBytes = 0;

	if ( argc > 1 && !strcmp(argv[1],"-d") )
		f_debug = 1;				/* Enable debug messages */

	fifo_init("/dev/toot-mcu-fifo");
	i2c_init("/dev/i2c-1");			/* Open I2C controller */

	signal(SIGINT,sigint_handler);	/* Trap on SIGINT */
	
	input_read(&inputState);   		/* Read to set initial values */

	while ( !is_signaled )
	{

		timed_wait(0,16666,0); // 60 Hz Refresh
		
		copy_input_data(&inputState, &prevState);
		
		while (!is_signaled & ( input_read(&inputState) < 0 )) {}
		
		if ( inputChange(&inputState, &prevState) )
		{
			reportBytes = generateReport(&inputState, &prevState, reportBuffer, debugBuffer);
			write(fifo_fd, reportBuffer, reportBytes);

			if ( f_debug )
			{
				fputs(debugBuffer,stdout);
			}
		}
		else
		{
			continue;
		}
		
	}

	if(f_debug)
	{
		puts("Closing.\n");
	}	

	fifo_close("/dev/toot_mcu_fifo");
	i2c_close();
}

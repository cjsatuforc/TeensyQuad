#include <string.h>
#include <stdio.h>

#include "common.h"
#include "FreeRTOS.h"
#include "FreeRTOSConfig.h"
#include "portmacro.h"
#include "timers.h"
#include "i2c.h"
#include "flight.h"
#include "SFE_LSM9DS0.h"

// Define me if you want debugging, remove me for release!
//#define configASSERT( x )     if( ( x ) == 0 ) { taskDISABLE_INTERRUPTS(); for( ;; ); }

#define STARTUP_BLINK_COUNT		( 3 )
#define STARTUP_BLINK_PERIOD	( 100 )

#define LSM9DS0_XM				( 0x1D ) // Would be 0x1E if SDO_XM is LOW
#define LSM9DS0_G				( 0x6B ) // Would be 0x6A if SDO_G is LOW

#define LED_TICK_MS				( 100UL )

static stLSM9DS0_t lsm9dso_dvr;
static TimerHandle_t led_timer = NULL;
static TaskHandle_t led_task = NULL;

/**
 * @brief		Delay using a loop. Milliseconds are very approximate based on
 * 				trial and error for our clock speed. Interrupts with slow this
 * 				down.
 * @param[in]	ms		Delay in ms.
 */
static __inline__ void dumbdelay_ms( const uint32_t ms )
{
	uint32_t loops;
	uint32_t index;

	// Calc delay in clock cycles
	loops = ms * ( (uint32_t)mcg_clk_hz / 10000 );

	// Dumb delay
	for ( index = 0; index < loops; index++ );
}

/*!
 * \brief	Called by the system when a hard fault is encountered.
 * 			Flashes our led at 20hz indefinitely.
 */
void HardFault_Handler()
{
	for (;;)
	{
		// Do the "hard fault panic" dance
		GPIOC_PSOR = ( 1 << 5 );
		dumbdelay_ms( 50 );
		GPIOC_PCOR = ( 1 << 5 );
		dumbdelay_ms( 50 );
	}
}

/**
 * @brief		If enabled, this hook will be called in case of a stack
 * 				overflow.
 * @param[in]	pxTask		Task handle.
 * @param[in]	pcTaskName	Pointer to task name.
 */
void vApplicationStackOverflowHook( xTaskHandle pxTask, char *pcTaskName )
{
	/* This will get called if a stack overflow is detected during the context
	 switch.  Set configCHECK_FOR_STACK_OVERFLOWS to 2 to also check for stack
	 problems within nested interrupts, but only do this for debug purposes as
	 it will increase the context switch time. */
	(void)pxTask;
	(void)pcTaskName;
	taskDISABLE_INTERRUPTS();
	/* Write your code here ... */
	for(;;) {}
}

/**
 * @brief		If enabled, this hook will be called by the RTOS for every
 *				tick increment.
 */
void vApplicationTickHook( void )
{
	/* Called for every RTOS tick. */
	/* Write your code here ... */
}

/**
 * @brief		If enabled, this hook will be called when the RTOS is idle.
 *				This might be a good place to go into low power mode.
 */
void vApplicationIdleHook( void )
{
	/* Called whenever the RTOS is idle (from the IDLE task).
	 Here would be a good place to put the CPU into low power mode. */
	/* Write your code here ... */
}

/**
 * @brief		If enabled, the RTOS will call this hook in case memory
 *				allocation failed.
 */
void vApplicationMallocFailedHook( void )
{
	/* Called if a call to pvPortMalloc() fails because there is insufficient
	 free memory available in the FreeRTOS heap.  pvPortMalloc() is called
	 internally by FreeRTOS API functions that create tasks, queues, software
	 timers, and semaphores.  The size of the FreeRTOS heap is set by the
	 configTOTAL_HEAP_SIZE configuration constant in FreeRTOSConfig.h. */
	taskDISABLE_INTERRUPTS();
	/* Write your code here ... */
	for(;;) {}
}


/**
 * @brief		Initialises the onboard LED.
 */
static void init_led( void )
{
	// Initialise on board LED
	PORTC_PCR5 = PORT_PCR_MUX( 0x1 );	// LED is on PC5 (pin 13), config as GPIO (alt = 1)
	GPIOC_PDDR = ( 1 << 5 );			// make this an output pin
	GPIOC_PCOR = ( 1 << 5 );			// start with LED off
}

/**
 * @brief		Initialises the serial port module, baud rate=115200 8N1, hw
 * 				flow control disabled.
 * @param[in]	channel		UART module's base register pointer.
 */
void init_serial( const UART_MemMapPtr channel )
{
	int baud = 115200;
	register uint16_t ubd, brfa;
	uint8_t temp;

	// Initialise serial port pins
	PORTB_PCR16 = PORT_PCR_MUX( 0x3 );
	PORTB_PCR17 = PORT_PCR_MUX( 0x3 );

	/* Enable the clock to UART0 */
	SIM_SCGC4 |= SIM_SCGC4_UART0_MASK;

	/* Make sure that the transmitter and receiver are disabled while we
	* change settings.
	*/
	UART_C2_REG( channel ) &= ~( UART_C2_TE_MASK | UART_C2_RE_MASK );

	/* Configure the UART for 8-bit mode, no parity */
	/* We need all default settings, so entire register is cleared */
	UART_C1_REG( channel ) = 0;

	/* Calculate baud settings */
	ubd = (uint16_t)( ( mcg_clk_khz * 1000)/( baud * 16 ) );

	/* Save off the current value of the UARTx_BDH except for the SBR */
	temp = UART_BDH_REG( channel ) & ~( UART_BDH_SBR( 0x1F ) );
	UART_BDH_REG( channel ) = temp | UART_BDH_SBR( ( ( ubd & 0x1F00 ) >> 8 ) );
	UART_BDL_REG( channel ) = (uint8_t)( ubd & UART_BDL_SBR_MASK );

	/* Determine if a fractional divider is needed to get closer to the baud rate */
	brfa = ( ( ( mcg_clk_khz * 32000 ) / ( baud * 16 ) ) - ( ubd * 32 ) );

	/* Save off the current value of the UARTx_C4 register except for the BRFA */
	temp = UART_C4_REG( channel ) & ~( UART_C4_BRFA( 0x1F ) );
	UART_C4_REG( channel ) = temp | UART_C4_BRFA( brfa );

	/* Enable receiver and transmitter */
	UART_C2_REG( channel ) |= ( UART_C2_TE_MASK | UART_C2_RE_MASK );
}

void init_i2c( void )
{
	// Enable interrupt in NVIC and set priority to 0
	// See the vector channel assignments in K20P121M100SF2RM.pdf page 69
	// The interupt for I2C0 is number 79:
	// 24 / 32 = 0
	// 24 % 32 = 24
	// Therefore we choose the 0th register and set bit 24 (INT_I2C0 - 16)
	NVICICPR0 |= ( 1 << 24 );
	NVICISER0 |= ( 1 << 24 );
	NVICIP24 = 0x00;

	SIM_SCGC4 |= SIM_SCGC4_I2C0_MASK;
	SIM_SCGC5 |= SIM_SCGC5_PORTB_MASK;

	PORTB_PCR2 = PORT_PCR_MUX(0x02) | PORT_PCR_ODE_MASK;
	PORTB_PCR3 = PORT_PCR_MUX(0x02) | PORT_PCR_ODE_MASK;

	// TODO check status?
	i2c_init( 0, 0x01, 0x20 );
}

/**
 * @brief		Get a character from the buffer.
 * @param[in]	channel		UART module's base register pointer.
 * @return		The character received from our FIFO.
 */
char uart_getchar( const UART_MemMapPtr channel )
{
	/* Wait until character has been received */
	while (!(UART_S1_REG(channel) & UART_S1_RDRF_MASK));

	/* Return the 8-bit data from the receiver */
	return UART_D_REG(channel);
}

/**
 * @brief		Put a character into the tx buffer.
 * @param[in]	channel		UART module's base register pointer.
 * @param[in]	ch			Character to send.
 */
static void uart_putchar( const UART_MemMapPtr channel, const char ch )
{
	/* Wait until space is available in the FIFO */
	while(!(UART_S1_REG(channel) & UART_S1_TDRE_MASK));

	/* Send the character */
	UART_D_REG(channel) = (uint8_t)ch;
}


int port_putchar( int c )
{
	uart_putchar( UART0_BASE_PTR, c );
	return 1;
}

#if 0
/**
 * @brief		Put a string into the tx buffer.
 * @param[in]	channel		UART module's base register pointer.
 * @param[in]	ch			Characters to send.
 */
static void uart_puts( UART_MemMapPtr channel, const char *const s )
{
	int i;

	for ( i = 0; i < strlen( s ); i++ )
	{
		uart_putchar( channel, s[i] );
	}
}
#endif

/**
 * @brief		Blinks a number of times with a given interval.
 * @param[in]	reps		Number of times to blink.
 * @param[in]	period_ms	Blink period (ms).
 */
static void blink( const int reps, const int period_ms )
{
	int halfperiod_ms = period_ms / 2;
	int idx;

	for ( idx = 0; idx < reps; idx ++ )
	{
		// Set LED
		GPIOC_PSOR = ( 1 << 5 );
		dumbdelay_ms( halfperiod_ms );

		// Clear LED
		GPIOC_PCOR = ( 1 << 5 );
		dumbdelay_ms( halfperiod_ms );
	}
}

/**
 * @brief		Runs recursive flight processing.
 * @param[in]	arg		Opaque pointer to our user data.
 */
static void taskhandler_flight( void *arg )
{
	for ( ;; )
	{
		// TODO: Replace with spart sleep
		vTaskDelay( 1000 );

		LSM9DS0_readAccel( &lsm9dso_dvr );

		printf( "Accel = %d, %d, %d\r\n", lsm9dso_dvr.ax, lsm9dso_dvr.ay, lsm9dso_dvr.az );

		// Process flight controller
		flight_process( 0, NULL, NULL );
	}
}

/**
 * @brief		Runs recursive comms processing.
 * @param[in]	arg		Opaque pointer to our user data.
 */
static void taskhandler_comms( void *arg )
{
	// Suspend ourselves forever!
	vTaskSuspend( NULL );
}

/**
 * @brief		Runs the LED diagnostics reporting.
 * @param[in]	arg		Opaque pointer to our user data.
 */
static void taskhandler_led( void *arg )
{
	uint16_t blink_sequence[] = { 500, 500 };
	size_t sequence_position = 0;
	size_t sequence_len = sizeof( blink_sequence ) / sizeof( blink_sequence[0] );
	uint16_t sequence_ctr = 0;

	xTimerStart( led_timer, 0 );

	for ( ; ; )
	{
		// Suspend ourselves until some nice person resumes us...
		vTaskSuspend( NULL );

		sequence_ctr += LED_TICK_MS;

		// Set the LED depending on where we are in the sequence
		if ( 0 == ( sequence_position % 2 ) )
		{
			// Set LED
			GPIOC_PSOR = ( 1 << 5 );
		}
		else
		{
			// Clear LED
			GPIOC_PCOR = ( 1 << 5 );
		}

		if ( sequence_ctr >= blink_sequence[ sequence_position ] )
		{
			sequence_ctr = 4;
			sequence_position++;

			if ( sequence_len == sequence_position )
			{
				sequence_position = 0;
			}
		}
	}
}

/**
 * @brief		Callback for our timer, controls the LED task.
 * @param[in]	xTimer		Timer handle.
 */
static void timerCallback( TimerHandle_t xTimer )
{
	// Resume the led task
	vTaskResume( led_task );
}


static void set_rotor_spd( const size_t rotor_number, const uint16_t spd )
{
	//printf( "Set rotor spd %d = %d\r\n", rotor_number, spd );
	return;
}

static uint16_t get_recvr_channel( const size_t channel_number )
{
	//printf( "Get rxr %d\r\n", channel_number );
	return 0;
}

static void write_byte( stLSM9DS0_t * stThis, uint8_t address, uint8_t subAddress, uint8_t data )
{
	//printf( "I2Cwr %d>%d\r\n", subAddress, data );
	i2c_write_byte( 0, address, subAddress, data );

	return;
}

static uint8_t read_byte( stLSM9DS0_t * stThis, uint8_t address, uint8_t subAddress )
{
	uint8_t data;

	//printf( "I2Crd %d\r\n", subAddress );
	i2c_read_byte( 0, address, subAddress, &data );

	return data;
}

static void read_bytes( stLSM9DS0_t * stThis, uint8_t address, uint8_t subAddress, uint8_t * dest, uint8_t count )
{
	//printf( "I2Crd %dx%d\r\n", subAddress, count );
	i2c_read_bytes( 0, address, subAddress, dest, count );

	return;
}

/**
** @brief		Entry point to program.
** @return		Error code.
*/
int main( void )
{
	// Initialise hardware and peripherals
	init_led();
	init_serial( UART0_BASE_PTR );
	init_i2c();

	// Initialise LSM driver and flight controller
	LSM9DS0_Setup( &lsm9dso_dvr, MODE_I2C, LSM9DS0_G, LSM9DS0_XM, write_byte, read_byte, read_bytes );
	flight_setup( set_rotor_spd, get_recvr_channel );

	// Flash a little startup sequence, this isn't necessary at all, just nice
	// to see a familiar sign before things start breaking!
	blink( STARTUP_BLINK_COUNT, STARTUP_BLINK_PERIOD );

	// Say hello - printf is piped through the uart!
	printf( "Hello, World!\r\n" );

	LSM9DS0_begin( &lsm9dso_dvr );

	// Create our flight task
	xTaskCreate( taskhandler_flight,			// The task's callback function
				 "Flight",						// Task name
				 configMINIMAL_STACK_SIZE,		// We can specify different stack sizes for each task? Cool!
				 NULL,							// Parameter to pass to the callback function, we have nothhing to pass..
				 2,								// Priority, this is our only task so.. lets just use 0
				 NULL );						// We could put a pointer to a task handle here which will be filled in when the task is created

	// Create our comms task
	xTaskCreate( taskhandler_comms,				// The task's callback function
				 "Comms",						// Task name
				 configMINIMAL_STACK_SIZE,		// We can specify different stack sizes for each task? Cool!
				 NULL,							// Parameter to pass to the callback function, we have nothhing to pass..
				 1,								// Priority, this is our only task so.. lets just use 0
				 NULL );						// We could put a pointer to a task handle here which will be filled in when the task is created

	// Create our comms task
	xTaskCreate( taskhandler_led,				// The task's callback function
				 "LED_Diags",					// Task name
				 configMINIMAL_STACK_SIZE,		// We can specify different stack sizes for each task? Cool!
				 NULL,							// Parameter to pass to the callback function, we have nothhing to pass..
				 0,								// Priority, this is our only task so.. lets just use 0
				 &led_task );					// We could put a pointer to a task handle here which will be filled in when the task is created

	// Create a timer with a period of 500ms
	led_timer = xTimerCreate( "LED_Diags_Timer", 				/* A text name, purely to help debugging. */
							  ( LED_TICK_MS / portTICK_PERIOD_MS ),	/* The timer period, in this case 500ms. */
							  pdTRUE,							/* We want this to be a recurring timer so set uxAutoReload to pdTRUE. */
							  ( void * ) 0,						/* The ID is not used, so can be set to anything. */
							  timerCallback );					/* The callback function that is called when the timer expires. */

	// Start the tasks and timer running, this should never return as FreeRTOS
	// will branch directly into the idle task.
	vTaskStartScheduler();

	// We should never get here, this return is just to keep the compiler happy
	return 0;
}

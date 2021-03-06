/* ************************************************************************** **
 * Includes
 * ************************************************************************** */
#include "task_flight.h"

#include <stdint.h>			// std types
#include <stdbool.h>		// bool definition
#include <stddef.h>			// size_t
#include <string.h>			// memset & friends
#include <stdio.h>			// printf & friends

#include "FreeRTOS.h"		// FreeRTOS
#include "FreeRTOSConfig.h"	// FreeRTOS portable config
#include "portmacro.h"		// Portable functions
#include "timers.h"			// FreeRTOS timers
#include "queue.h"			// FreeRTOS queues

#include "config.h"			// Board specific config
#include "SFE_LSM9DS0.h"	// LSM9DS0 driver
#include "flight.h"			// Flight controller
#include "vector3f.h"		// vector3f_t
#include "io_driver.h"		// IO driver
#include "SFE_LSM9DS0.h"	// LSM9DS0 driver
#include "i2c.h"			// I2C device driver
#include "IPC_types.h"		// stFlightDetails_t
#include "params.h"			// System parameter access
#include "pubsub.h"			// IPC publish-subscribe

/* ************************************************************************** **
 * Macros and Defines
 * ************************************************************************** */
#define FLIGHT_TICK_MS			( 10UL )
#define mArrayLen( x )			( sizeof( x ) / sizeof( x[0] ) )

#define LSM9DS0_XM				( 0x1D ) // Would be 0x1E if SDO_XM is LOW
#define LSM9DS0_G				( 0x6B ) // Would be 0x6A if SDO_G is LOW

#define PI						( 3.14159265359f )
#define RAD2DEG					( 180 / PI )
#define DEG2RAD					( PI / 180 )

/* ************************************************************************** **
 * Typedefs
 * ************************************************************************** */
typedef struct
{
	int16_t iTemp;
	vector3f_t stBias;

} stGyroBiasTableEntry_t;

/* ************************************************************************** **
 * Function Prototypes
 * ************************************************************************** */
/**
 * @brief		Entry point for the fligh task.
 * @param[in]	arg		Opaque pointer to user data.
 */
static void TaskHandler( void *arg );

/**
 * @brief		Callback for the timer.
 * @param[in]	xTimer	Timer handle.
 */
static void TimerHandler( TimerHandle_t xTimer );

static void UpdateParameters( void );

#if 0
/**
 * @brief		Prints some debug to stdout.
 * @param[in]	accel	Current accelerometer values.
 * @param[in]	gyro	Current gyro values.
 */
static void PrintDebug( vector3f_t accel, vector3f_t gyro, int16_t temp );
#endif

/**
 * @brief		Returns the gyro bias for a given temperature performing a table
 * 				lookup and a linear interpolation.
 * @param[in]	iTemp	Temperature to lookup.
 * @returns		The calculated bias vector.
 */
static vector3f_t GetBias( int16_t iTemp );

static void write_byte( stLSM9DS0_t * stThis, uint8_t address, uint8_t subAddress, uint8_t data );
static uint8_t read_byte( stLSM9DS0_t * stThis, uint8_t address, uint8_t subAddress );
static void read_bytes( stLSM9DS0_t * stThis, uint8_t address, uint8_t subAddress, uint8_t * dest, uint8_t count );

/* ************************************************************************** **
 * Local Variables
 * ************************************************************************** */
static TaskHandle_t xFlightTaskHandle = NULL;
static TimerHandle_t xFlightTimerHandle = NULL;
static stLSM9DS0_t stImu;
static vector3f_t stAverageGyro;

static const uint16_t auiLedPatternFlight[] = { 500, 500 };

// Parameters
static stPARAM_t *pstTrimRoll;
static stPARAM_t *pstTrimPitch;
static stPARAM_t *pstTrimYaw;
static stPARAM_t *pstPidGainRateP;
static stPARAM_t *pstPidGainRateD;
static stPARAM_t *pstPidGainAngleP;
static stPARAM_t *pstPidGainRateYawP;
static stPARAM_t *pstPidGainRateYawD;

static uint16_t uiWhoAmI;

static bool bDone;
static stFlightDetails_t stFlightDetails;

/* ************************************************************************** **
 * API Functions
 * ************************************************************************** */

/* ************************************************************************** */
void TASK_FLIGHT_Create( void )
{
	// Create our flight task
	xTaskCreate( TaskHandler,					// The task's callback function
				 "TASK_Flight",					// Task name
				 500,							// Make the flight controller's stack large as we are likely to do many function calls
				 NULL,							// Parameter to pass to the callback function, we have nothhing to pass..
				 2,								// Priority, this is our only task so.. lets just use 0
				 &xFlightTaskHandle );			// We could put a pointer to a task handle here which will be filled in when the task is created

	// Create a timer for our flight task
	xFlightTimerHandle = xTimerCreate( "TIMER_Flight", 							// A text name, purely to help debugging.
									   ( FLIGHT_TICK_MS / portTICK_PERIOD_MS ),	// The timer period, in this case 500ms.
									   pdTRUE,										// We want this to be a recurring timer so set uxAutoReload to pdTRUE.
									   ( void * ) 0,								// The ID is not used, so can be set to anything.
									   TimerHandler );								// The callback function that is called when the timer expires.

	memset( &stAverageGyro, 0, sizeof( stAverageGyro ) );

	return;
}

/* ************************************************************************** **
 * Local Functions
 * ************************************************************************** */

/* ************************************************************************** */
static void TaskHandler( void *arg )
{
	vector3f_t accel;
	vector3f_t gyro;
	vector3f_t mag;
	vector3f_t stGyroBias;
	stReceiverInput_t stReceiverInputs;
	stMotorDemands_t stMotorDemands;
	stLedPattern_t stLedPattern;
	uint8_t uiCount;

	memset( &stFlightDetails, 0, sizeof( stFlightDetails ) );

	// Start our timer which will resume this task accurately on a tick
	xTimerStart( xFlightTimerHandle, 0 );

	// Set the LED to blink with the "flying" pattern
	memcpy( stLedPattern.auiPattern, auiLedPatternFlight, sizeof( auiLedPatternFlight ) );
	stLedPattern.sPatternLen = mArrayLen( auiLedPatternFlight );
	PUBSUB_Publish( TOPIC_LED_PATTERN, &stLedPattern );

	// Initialise LSM driver
	LSM9DS0_Setup( &stImu, MODE_I2C, LSM9DS0_G, LSM9DS0_XM, write_byte, read_byte, read_bytes );

	// Initialise the IMU
	uiWhoAmI = LSM9DS0_begin_adv( &stImu,
								  G_SCALE_500DPS,
								  A_SCALE_8G,
								  M_SCALE_4GS,
								  G_ODR_380_BW_100,
								  A_ODR_800,
								  M_ODR_25 );

	// Print whoami to serve as a comms sanity check
	printf( "LSM: Whoami=%X - should be 49D4\r\n", (int)uiWhoAmI );

	// Initialize the flight controller module
	flight_setup();

	// Search for and store pointers to system parameters for quick access later
	// This makes the assumption that parameters cannot come and go at runtime
	pstTrimRoll = PARAM_FindParamByName( "TrimRoll", 0, NULL );
	pstTrimPitch = PARAM_FindParamByName( "TrimPitch", 0, NULL );
	pstTrimYaw = PARAM_FindParamByName( "TrimYaw", 0, NULL );
	pstPidGainRateP = PARAM_FindParamByName( "PIDGainRate_P", 0, NULL );
	pstPidGainRateD = PARAM_FindParamByName( "PIDGainRate_D", 0, NULL );
	pstPidGainAngleP = PARAM_FindParamByName( "PIDGainAngle_P", 0, NULL );
	pstPidGainRateYawP = PARAM_FindParamByName( "PIDGainRateYaw_P", 0, NULL );
	pstPidGainRateYawD = PARAM_FindParamByName( "PIDGainRateYaw_D", 0, NULL );

	for ( ; ; )
	{
		bDone = false;
		stFlightDetails.uiFlightRunCount++;

		// Collects trim and PID gain updates from the parameters and passes
		// them into the flight controller if they have updated
		UpdateParameters();

		// Read the gyro fifo
		uiCount = LSM9DS0_fifoCountGyro( &stImu );
		stFlightDetails.uiGyroSampleCount += uiCount;

		while ( uiCount-- > 0 )
		{
			LSM9DS0_readGyro( &stImu );

			// Read off gyro values scaling into rad/sec
			gyro.x = LSM9DS0_calcGyro( &stImu, stImu.gx );
			gyro.y = LSM9DS0_calcGyro( &stImu, stImu.gy );
			gyro.z = LSM9DS0_calcGyro( &stImu, stImu.gz );
		}

		// Read the accel fifo
		uiCount = LSM9DS0_fifoCountAccel( &stImu );
		stFlightDetails.uiAccelSampleCount += uiCount;

		while ( uiCount-- > 0 )
		{
			LSM9DS0_readAccel( &stImu );

			// Read off gyro values scaling into rad/sec
			accel.x = LSM9DS0_calcAccel( &stImu, stImu.ax );
			accel.y = LSM9DS0_calcAccel( &stImu, stImu.ay );
			accel.z = LSM9DS0_calcAccel( &stImu, stImu.az );
		}

#if 1

		// Read the latest accel and gyro values
		//LSM9DS0_readAccel( &stImu );
		//LSM9DS0_readGyro( &stImu );
		//LSM9DS0_readMag( &stImu );

		// Scale the gyro values into rad/sec
		mag.x = LSM9DS0_calcMag( &stImu, stImu.mx );
		mag.y = LSM9DS0_calcMag( &stImu, stImu.my );
		mag.z = LSM9DS0_calcMag( &stImu, stImu.mz );

		// Calculate and apply gyro bias
		LSM9DS0_readTemp( &stImu );
		stGyroBias = GetBias( stImu.temperature );
		gyro = VECTOR3F_Subtract( gyro, stGyroBias );

		// The value we get out of the gyro is in degrees/sec but we want it in
		// rad/sec so lets convert it now.
		gyro.x *= DEG2RAD;
		gyro.y *= DEG2RAD;
		gyro.z *= DEG2RAD;

		// Work out receiver input values as floats
		stReceiverInputs.fRoll = ( (float)( (int32_t)IODRIVER_GetInputPulseWidth( CFG_RECEIVER_ROLL ) - RECEIVER_CENTER ) ) / ( RECEIVER_RANGE / 2 );
		stReceiverInputs.fPitch = ( (float)( (int32_t)IODRIVER_GetInputPulseWidth( CFG_RECEIVER_PITCH ) - RECEIVER_CENTER ) ) / ( RECEIVER_RANGE / 2 );
		stReceiverInputs.fThrottle = ( (float)IODRIVER_GetInputPulseWidth( CFG_RECEIVER_THROTTLE ) ) / RECEIVER_RANGE;
		stReceiverInputs.fYaw = ( (float)( (int32_t)IODRIVER_GetInputPulseWidth( CFG_RECEIVER_YAW ) - RECEIVER_CENTER ) ) / ( RECEIVER_RANGE / 2 );
		stReceiverInputs.fVarA = ( (float)IODRIVER_GetInputPulseWidth( CFG_RECEIVER_VRA ) ) / RECEIVER_RANGE;
		stReceiverInputs.fVarB = ( (float)IODRIVER_GetInputPulseWidth( CFG_RECEIVER_VRB ) ) / RECEIVER_RANGE;

		// Process flight controller
		flight_process( FLIGHT_TICK_MS,
						&accel,
						&gyro,
						&mag,
						&stReceiverInputs,
						&stMotorDemands );

		// Set the motor outputs based on the results from the flight controller
		IODRIVER_SetOutputPulseWidth( CFG_MOTOR_FL, (uint32_t)( stMotorDemands.fFL * RECEIVER_RANGE ) );
		IODRIVER_SetOutputPulseWidth( CFG_MOTOR_FR, (uint32_t)( stMotorDemands.fFR * RECEIVER_RANGE ) );
		IODRIVER_SetOutputPulseWidth( CFG_MOTOR_RL, (uint32_t)( stMotorDemands.fRL * RECEIVER_RANGE ) );
		IODRIVER_SetOutputPulseWidth( CFG_MOTOR_RR, (uint32_t)( stMotorDemands.fRR * RECEIVER_RANGE ) );

#endif

		// Publish flight details
		FLIGHT_GetRotation( &stFlightDetails.stAttitude );
		stFlightDetails.stAttitudeRate = gyro;
		PUBSUB_Publish( TOPIC_FLIGHT_DETAILS, &stFlightDetails );

		// Suspend until our timer wakes us up again
		bDone = true;
		vTaskSuspend( NULL );
	}
}

/* ************************************************************************** */
static void UpdateParameters( void )
{
	vector3f_t stTrim = { 0, };

	// Copy in each parameter if they exist
	if ( pstTrimRoll )
	{
		stTrim.x = pstTrimRoll->fValue;
	}

	if ( pstTrimPitch )
	{
		stTrim.y = pstTrimPitch->fValue;
	}

	if ( pstTrimYaw )
	{
		stTrim.y = pstTrimPitch->fValue;
	}

	// Set the trim in the flight controller
	FLIGHT_SetTrim( &stTrim );

	// Update the PID gains of the flight controller (the ones that matter!)
	FLIGHT_SetPidGains( pstPidGainRateP->fValue, pstPidGainRateD->fValue, pstPidGainAngleP->fValue );

	return;
}

/* ************************************************************************** */
static void TimerHandler( TimerHandle_t xTimer )
{
	if ( bDone == false )
	{
		stFlightDetails.uiFlightTaskMissed++;
	}
	else
	{
		vTaskResume( xFlightTaskHandle );
	}

	return;
}

/* ************************************************************************** */
static vector3f_t GetBias( int16_t iTemp )
{
	static const stGyroBiasTableEntry_t astGyroBiasTable[] =
	{
		{ 3, { -0.618, 0.900, 1.000 } },
		{ 43, { -0.500, 0.380, 4.200 } },
	};

	uint16_t uiIndex;
	int16_t iTempRange;
	int16_t iTempDelta;
	float iTempScale;
	vector3f_t stBiasRange;
	vector3f_t stBiasDelta;
	const stGyroBiasTableEntry_t *pstBiasUpper;
	const stGyroBiasTableEntry_t *pstBiasLower;

	// Linear interpolation - TODO might be a good idea to do a bsearch here
	for ( uiIndex = 0; uiIndex < mArrayLen( astGyroBiasTable ); uiIndex++ )
	{
		if ( iTemp < astGyroBiasTable[ uiIndex ].iTemp )
		{
			break;
		}
	}

	if ( uiIndex == 0 || uiIndex == mArrayLen( astGyroBiasTable ) )
	{
		// Temp too low or too high - no point in interpolating
		return astGyroBiasTable[ uiIndex ].stBias;
	}
	else
	{
		// Store pointers to the upper and lower bounds of the interpolation
		pstBiasLower = &astGyroBiasTable[ uiIndex - 1 ];
		pstBiasUpper = &astGyroBiasTable[ uiIndex ];

		// Linear interpolation
		iTempRange = pstBiasUpper->iTemp - pstBiasLower->iTemp;
		iTempDelta = iTemp - pstBiasLower->iTemp;
		iTempScale = (float)iTempDelta / (float)iTempRange;

		// Calculate the range vector
		stBiasRange.x = pstBiasUpper->stBias.x - pstBiasLower->stBias.x;
		stBiasRange.y = pstBiasUpper->stBias.y - pstBiasLower->stBias.y;
		stBiasRange.z = pstBiasUpper->stBias.z - pstBiasLower->stBias.z;

		// Calculate the delta vector
		stBiasDelta.x = stBiasRange.x * iTempScale;
		stBiasDelta.y = stBiasRange.y * iTempScale;
		stBiasDelta.z = stBiasRange.z * iTempScale;

		// Add the ranges
		stBiasDelta.x += pstBiasLower->stBias.x;
		stBiasDelta.y += pstBiasLower->stBias.y;
		stBiasDelta.z += pstBiasLower->stBias.z;

		return stBiasDelta;
	}
}

#if 0
/* ************************************************************************** */
static void PrintDebug( vector3f_t accel, vector3f_t gyro, int16_t temp )
{
	vector3f_t stBias = GetBias( temp );

#if 0
	// Print accel and gyro to stdout - values are * 1000 */
	printf( "Accel = %d, %d, %d\r\n",
			(int)( accel.x * 1000 ),
			(int)( accel.y * 1000 ),
			(int)( accel.z * 1000 ) );
#endif

	stAverageGyro = VECTOR3F_Add( VECTOR3F_Scale( stAverageGyro, 0.8 ),
								  VECTOR3F_Scale( VECTOR3F_Subtract( gyro, stBias ), 0.2 ) );

	/*
	printf( "Gyro = %d, %d, %d %d\r\n",
			(int)( stAverageGyro.x * 1000 ),
			(int)( stAverageGyro.y * 1000 ),
			(int)( stAverageGyro.z * 1000 ),
			(int)( temp ));
			*/

	printf( "Gyro = %d, %d, %d %d\r\n",
				(int)( ( stAverageGyro.x ) * 1000 ),
				(int)( ( stAverageGyro.y ) * 1000 ),
				(int)( ( stAverageGyro.z ) * 1000 ),
				(int)( temp ));

#if 0
	// Print receiver inputs to stdout
	for( i = 0; i < RECEIVER_NUM_CHAN_IN; i ++ )
	{
		IODRIVER_GetInputPulseWidth( i, &inputs[i] );
		printf( "Receiver input %d = %d\r\n", i, (int)inputs[i] );
	}
#endif
}

#endif

/* ************************************************************************** */
static void write_byte( stLSM9DS0_t * stThis, uint8_t address, uint8_t subAddress, uint8_t data )
{
	//printf( "I2Cwr %d>%d\r\n", subAddress, data );
	i2c_write_byte( 0, address, subAddress, data );

	return;
}

/* ************************************************************************** */
static uint8_t read_byte( stLSM9DS0_t * stThis, uint8_t address, uint8_t subAddress )
{
	uint8_t data;

	i2c_read_byte( 0, address, subAddress, &data );

	return data;
}

/* ************************************************************************** */
static void read_bytes( stLSM9DS0_t * stThis, uint8_t address, uint8_t subAddress, uint8_t * dest, uint8_t count )
{
	i2c_read_bytes( 0, address, subAddress, dest, count );

	return;
}


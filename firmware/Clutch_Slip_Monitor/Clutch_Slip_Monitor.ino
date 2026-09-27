// SPDX-FileCopyrightText: Johannes Stockhammer
// SPDX-License-Identifier: Apache-2.0
/*
 * Clutch_Slip_Monitor.ino
 *
 *  Author: Johannes Stockhammer
 *
 * Version:		1.04
 * Hardware:	Arduino Due (SAM3X8E, 84 MHz), Adafruit 3.5" TFT 480x320 (HX8357D, SPI),
 *				boards signal conditioning and ignition amplifier
 * Software:	Arduino IDE (Arduino SAM Boards)
 *
 * Description:
 *	Clutch slip monitor for an engine test bench.
 *	Engine rpm from the ignition signal (1 pulse per revolution) at TIOA0,
 *	test bench rpm from the Hall sensor at the trigger wheel (18 teeth) at TIOA1.
 *	A button teaches in the ratio engine/test bench, after that the
 *	rpm difference (Ndiff) is monitored. If Ndiff stays above the limit for more than 100ms,
 *	the slip output is activated for at least 1s.
 *	Display: ratio, bar engine rpm and bar Ndiff on the TFT.
 */

//Includes
	//Arduino Libraries
		#include <SPI.h>
		#include <stdint.h>
	//External Libraries (Adafruit, install with the Library Manager)
		#include "Adafruit_GFX.h"
		#include "Adafruit_HX8357.h"

//Defines
	//Software
		#define SOFTWARE_VERSION	"1.04"

	//TFT Display IO-Lines (Hardware SPI at the SPI header)
		#define TFT_CS				10			//Digital Pin 10
		#define TFT_DC				11			//Digital Pin 11
		#define TFT_RST				12			//Digital Pin 12

	//Pins (port bits on the SAM3X8E)
		#define BIT_ERPM			25			//PB25 = Digital Pin 2 = TIOA0 (Peripheral B), engine rpm
		#define BIT_BRPM			2			//PA2 = Analog Pin 7 = TIOA1 (Peripheral A), test bench rpm
		#define BIT_BUTTON			28			//PC28 = Digital Pin 3, button "teach in ratio"
		#define BIT_SLIP			17			//PB17 = Analog Pin 8, slip output (LOW = slip)

	//Peripheral IDs (PMC_PCER0)
		#define ID_PIOC_BIT			13			//PIOC
		#define ID_TC0_BIT			27			//TC0 (module 0, channel 0)
		#define ID_TC1_BIT			28			//TC1 (module 0, channel 1)
		#define ID_TC2_BIT			29			//TC2 (module 0, channel 2)

	//Tick timer (TC2)
		#define TC_CLOCK			42000000UL	//TIMER_CLOCK1 = MCK/2 = 84MHz/2
		#define TICK_US				500			//Tick every 500µs
		#define TICK_RC				(TC_CLOCK/1000000UL*TICK_US)	//RC value for 500µs = 21000
		#define MS_TO_TICKS(ms)		((ms)*1000UL/TICK_US)		//Time in ms -> number of ticks
		#define TIME_100ms			MS_TO_TICKS(100)			//= 200
		#define TIME_1sec			MS_TO_TICKS(1000)			//= 2000
		#define TIME_5sec			MS_TO_TICKS(5000)			//= 10000

	//Rpm calculation
		#define RPM_NUMERATOR		(60UL*TC_CLOCK)				//rpm = 60 * 42MHz / period[ticks] = 2520000000 / period
		#define TRIGGER_WHEEL_TEETH	18							//Teeth on the trigger wheel (Hall sensor)
		#define TIMEOUT_STANDSTILL	50000000UL					//approx. 1.19s without pulse (50e6 / 42MHz) -> rpm = 0
		#define FILTER_NEW			0.04f						//Weight of the new reading (moving average)
		#define FILTER_OLD			0.96f						//Weight of the old average

	//Slip detection
		#define NDIFF_LIMIT			100			//Rpm difference in rpm above which slip is detected

	//TFT display
		#define TFT_MAINWINDOW		1			//ID main screen (tft_lastwindow)

	//States button
		#define BUTTON_OPEN			0			//Button not pressed, wait for press
		#define BUTTON_DEBOUNCE		1			//Button pressed, debounce time running

	//States display
		#define DISPLAY_MAINSCREEN		0		//Main screen with bars
		#define DISPLAY_MEASURE_START	1		//Start teaching in the ratio
		#define DISPLAY_MEASURE			2		//Teach-in running (5s yellow screen)

	//States slip detection
		#define NDIFF_OK			0			//No slip
		#define NDIFF_CHECK			1			//Ndiff above limit, confirm for 100ms
		#define NDIFF_SLIP			2			//Slip detected, set output
		#define NDIFF_HOLD			3			//Hold output for at least 1s

//TFT init
	//Display init
		//default Hardware SPI is used
		Adafruit_HX8357 tft = Adafruit_HX8357(TFT_CS, TFT_DC, TFT_RST);

//----------------------------------------------------------//

//Variables
	//RPM
		int32_t rpm_ndiff=0;							//Rpm difference engine - test bench*ratio (can be negative)
		float rpm_ratio=0;								//Taught ratio engine/test bench
		volatile uint16_t rpm_timer=0;					//Timer for the teach-in

	//RPM1 - Engine RPM - Ignition
		float erpm_average=0;							//Filtered engine rpm in rpm
		volatile uint32_t erpm_timer_val=0;				//Period from TC0 RA
		volatile bool erpm_interrupt_state=0;			//New reading available

	//RPM2 - Bench RPM - Hall sensor
		float brpm_average=0;							//Filtered test bench rpm in rpm
		volatile uint32_t brpm_timer_val=0;				//Period from TC1 RA
		volatile bool brpm_interrupt_state=0;			//New reading available

	//Ndiff detection
		volatile uint16_t ndiff_timer=0;				//Timer for confirmation/hold time
		uint8_t ndiff_state=NDIFF_OK;

	//TFT
		uint32_t tft_x1=0, tft_x2=0;					//Bar length engine rpm / Ndiff in pixels
		uint8_t tft_lastwindow=0, tft_val=0;			//Last drawn screen / state in the screen
		volatile uint16_t tft_timer=0;					//Timer for bar update

	//Button teach in ratio
		volatile uint8_t switch_timer=0;				//Debounce timer
		uint8_t switch_state=BUTTON_OPEN;
		bool switch_pressed=0;							//Debounced state

	//Display
		uint8_t display_state=DISPLAY_MAINSCREEN;

//----------------------------------------------------------//

//Prototypes
void TFT_Main_Screen();

int main()
{
	//Initial Functions
		REG_WDT_MR = (1 << 15);				//WDDIS: watchdog off (newer SAM cores only disable it in their own main()), WDT_MR can only be written once
		SystemInit();						//Atmel SAM init routine
		init();								//Arduino init routine (SysTick, pins), needed for the Arduino libraries

	//TFT Display init
		tft.begin();						//Display type HX8357D is set in the constructor
		tft.setRotation(3);					//Landscape

	//Inputs/Outputs
		//Inputs
			//Engine RPM - PB25 - Digital Pin 2
				REG_PIOB_PDR = (1 << BIT_ERPM);		//PIO controller disabled (pin to peripheral)
				REG_PIOB_PUDR = (1 << BIT_ERPM);	//Pull up disabled
				REG_PIOB_ABSR |= (1 << BIT_ERPM);	//Peripheral B function (TIOA0), ABSR is read/write

			//Bench RPM - PA2 - Analog Pin 7
				REG_PIOA_PDR = (1 << BIT_BRPM);		//PIO controller disabled (pin to peripheral)
				REG_PIOA_PUDR = (1 << BIT_BRPM);	//Pull up disabled
				REG_PIOA_ABSR &= ~(1 << BIT_BRPM);	//Peripheral A function (TIOA1)

			//Button teach in ratio - PC28 - Digital Pin 3
				REG_PMC_PCER0 = (1 << ID_PIOC_BIT);	//PIOC clock on (for input and glitch filter)
				REG_PIOC_PER = (1 << BIT_BUTTON);	//PIO controller enabled
				REG_PIOC_ODR = (1 << BIT_BUTTON);	//Output disabled -> input
				REG_PIOC_IFER = (1 << BIT_BUTTON);	//Input glitch filter enabled
				REG_PIOC_PUER = (1 << BIT_BUTTON);	//Pull up enabled (button switches to GND)

		//Outputs
			//Slip output - PB17 - Analog Pin 8
				REG_PIOB_PER = (1 << BIT_SLIP);		//PIO controller enabled
				REG_PIOB_OER = (1 << BIT_SLIP);		//Output enabled
				REG_PIOB_SODR = (1 << BIT_SLIP);	//HIGH = no slip

	//Timer
		//TC0 - Engine RPM (Capture, period between two falling edges at TIOA0)
			REG_PMC_PCER0 = (1 << ID_TC0_BIT);								//TC0 clock on
			REG_TC0_CMR0 &= ~( (1 << 0)|(1 << 1)|(1 << 2) );				//TC0 Clock Selection TIMER_CLOCK1 (MCK/2)
			REG_TC0_CMR0 &= ~(1 << 3);										//TC0 Counter incremented on rising Edge
			REG_TC0_CMR0 &= ~( (1 << 4)|(1 << 5) );							//TC0 Burst Signal selection NONE
			REG_TC0_CMR0 &= ~(1 << 6);										//TC0 Counter not stopped with RB loading
			REG_TC0_CMR0 &= ~(1 << 7);										//TC0 Counter not disabled with RB loading
			REG_TC0_CMR0 &= ~(1 << 8); REG_TC0_CMR0 |= (1 << 9);			//TC0 External Trigger Edge on falling Edge (Counter Reset)
			REG_TC0_CMR0 |= (1 << 10);										//TC0 TIOA is used as external Trigger
			REG_TC0_CMR0 &= ~(1 << 14);										//TC0 RC Compare has no effect
			REG_TC0_CMR0 &= ~(1 << 15);										//TC0 Capture Mode
			REG_TC0_CMR0 &= ~(1 << 16); REG_TC0_CMR0 |= (1 << 17);			//TC0 RA loading on falling Edge of TIOA
			REG_TC0_CMR0 &= ~( (1 << 18)|(1 << 19) );						//TC0 No RB loading
			REG_TC0_IER0 = (1 << 5);										//TC0 Interrupt on RA loading (LDRAS)
			NVIC_SetPriority(TC0_IRQn,1);									//TC0 interrupt priority
			NVIC_EnableIRQ(TC0_IRQn);										//TC0 interrupt enabled
			REG_TC0_CCR0 = (1 << 0)|(1 << 2);								//TC0 Clock Enable + Software Trigger (start counter)

		//TC1 - Bench RPM (Capture, period between two falling edges at TIOA1)
			REG_PMC_PCER0 = (1 << ID_TC1_BIT);								//TC1 clock on
			REG_TC0_CMR1 &= ~( (1 << 0)|(1 << 1)|(1 << 2) );				//TC1 Clock Selection TIMER_CLOCK1 (MCK/2)
			REG_TC0_CMR1 &= ~(1 << 3);										//TC1 Counter incremented on rising Edge
			REG_TC0_CMR1 &= ~( (1 << 4)|(1 << 5) );							//TC1 Burst Signal selection NONE
			REG_TC0_CMR1 &= ~(1 << 6);										//TC1 Counter not stopped with RB loading
			REG_TC0_CMR1 &= ~(1 << 7);										//TC1 Counter not disabled with RB loading
			REG_TC0_CMR1 &= ~(1 << 8); REG_TC0_CMR1 |= (1 << 9);			//TC1 External Trigger Edge on falling Edge (Counter Reset)
			REG_TC0_CMR1 |= (1 << 10);										//TC1 TIOA is used as external Trigger
			REG_TC0_CMR1 &= ~(1 << 14);										//TC1 RC Compare has no effect
			REG_TC0_CMR1 &= ~(1 << 15);										//TC1 Capture Mode
			REG_TC0_CMR1 &= ~(1 << 16); REG_TC0_CMR1 |= (1 << 17);			//TC1 RA loading on falling Edge of TIOA
			REG_TC0_CMR1 &= ~( (1 << 18)|(1 << 19) );						//TC1 No RB loading
			REG_TC0_IER1 = (1 << 5);										//TC1 Interrupt on RA loading (LDRAS)
			NVIC_SetPriority(TC1_IRQn,0);									//TC1 interrupt priority (highest, many pulses per revolution)
			NVIC_EnableIRQ(TC1_IRQn);										//TC1 interrupt enabled
			REG_TC0_CCR1 = (1 << 0)|(1 << 2);								//TC1 Clock Enable + Software Trigger (start counter)

		//TC2 - Periodic Interrupt (tick every 500µs)
			REG_PMC_PCER0 = (1 << ID_TC2_BIT);								//TC2 clock on
			REG_TC0_CMR2 &= ~( (1 << 0)|(1 << 1)|(1 << 2) );				//TC2 Clock Selection TIMER_CLOCK1 (MCK/2)
			REG_TC0_CMR2 &= ~(1 << 3);										//TC2 Counter incremented on rising Edge
			REG_TC0_CMR2 &= ~( (1 << 4)|(1 << 5) );							//TC2 Burst Signal selection NONE
			REG_TC0_CMR2 &= ~(1 << 6);										//TC2 Counter not stopped with RB loading
			REG_TC0_CMR2 &= ~(1 << 7);										//TC2 Counter not disabled with RB loading
			REG_TC0_CMR2 &= ~( (1 << 8)|(1 << 9) );							//TC2 External Trigger Edge NONE
			REG_TC0_CMR2 &= ~(1 << 10);										//TC2 External Trigger Selection TIOB (not used)
			REG_TC0_CMR2 |= (1 << 14);										//TC2 RC Compare Trigger (counter reset at RC)
			REG_TC0_CMR2 &= ~(1 << 15);										//TC2 Capture Mode
			REG_TC0_CMR2 &= ~( (1 << 16)|(1 << 17) );						//TC2 RA Loading Edge NONE
			REG_TC0_CMR2 &= ~( (1 << 18)|(1 << 19) );						//TC2 RB Loading Edge NONE
			REG_TC0_RC2 = TICK_RC;											//21000 / 42MHz = 500µs
			REG_TC0_IER2 = (1 << 4);										//TC2 Interrupt on RC Compare (CPCS)
			NVIC_SetPriority(TC2_IRQn,2);									//TC2 interrupt priority
			NVIC_EnableIRQ(TC2_IRQn);										//TC2 interrupt enabled
			REG_TC0_CCR2 = (1 << 0)|(1 << 2);								//TC2 Clock Enable + Software Trigger (start counter)

	while(1)	//Main loop
	{
		//Rpm difference
			rpm_ndiff = int32_t(erpm_average - brpm_average*rpm_ratio);	//Engine rpm minus expected engine rpm from test bench

		//Debounce button (LOW = pressed because of pull-up)
			switch(switch_state)
			{
				case BUTTON_OPEN:		if( !(REG_PIOC_PDSR & (1 << BIT_BUTTON)) )		//If button pressed
										{
											switch_timer=TIME_100ms;					//Start debounce timer
											switch_state=BUTTON_DEBOUNCE;
										}
										else switch_pressed=0;							//Button not pressed
										break;

				case BUTTON_DEBOUNCE:	if( REG_PIOC_PDSR & (1 << BIT_BUTTON) )			//Button released -> bounce
										{
											switch_pressed=0;
											switch_state=BUTTON_OPEN;
										}
										else if(!switch_timer)							//Pressed for 100ms without interruption
										{
											switch_pressed=1;
											switch_state=BUTTON_OPEN;
										}
										break;
			}//end switch switch_state

		//Display and teach-in of the ratio
			switch(display_state)
			{
				case DISPLAY_MAINSCREEN:	TFT_Main_Screen();								//Draw/update main screen
											if(switch_pressed) display_state=DISPLAY_MEASURE_START;
											break;

				case DISPLAY_MEASURE_START:	rpm_timer=TIME_5sec;							//Wait 5s until the averages have settled
											tft.fillScreen(HX8357_YELLOW);					//Yellow = teach-in running
											display_state=DISPLAY_MEASURE;
											break;

				case DISPLAY_MEASURE:		if(!rpm_timer)									//Time expired
											{
												if( (erpm_average>0)&&(brpm_average>0) )	//Only teach in with valid rpm
												{
													rpm_ratio = erpm_average/brpm_average;	//Ratio engine/test bench
												}
												tft_lastwindow=0;							//Redraw main screen
												display_state=DISPLAY_MAINSCREEN;
											}
											break;
			}//end switch display_state

		//Slip detection
			switch(ndiff_state)
			{
				case NDIFF_OK:		if( (rpm_ndiff>NDIFF_LIMIT)&&(rpm_ratio>0) )	//Only with taught ratio
									{
										ndiff_timer=TIME_100ms;							//Start confirmation time
										ndiff_state=NDIFF_CHECK;
									}
									break;

				case NDIFF_CHECK:	if(rpm_ndiff<=NDIFF_LIMIT) ndiff_state=NDIFF_OK;	//Only a short outlier
									else if(!ndiff_timer) ndiff_state=NDIFF_SLIP;		//100ms above the limit without interruption
									break;

				case NDIFF_SLIP:	ndiff_timer=TIME_1sec;								//Start minimum hold time
									REG_PIOB_CODR = (1 << BIT_SLIP);					//LOW = slip
									ndiff_state=NDIFF_HOLD;
									break;

				case NDIFF_HOLD:	if( (!ndiff_timer)&&(rpm_ndiff<=NDIFF_LIMIT) )		//Hold time over and no more slip
									{
										REG_PIOB_SODR = (1 << BIT_SLIP);				//HIGH = no slip
										ndiff_state=NDIFF_OK;
									}
									break;
			}//end switch ndiff_state

		//Calculate engine rpm (1 ignition pulse per revolution)
			if(erpm_interrupt_state)
			{
				uint32_t period=erpm_timer_val;												//Local copy, ISR can overwrite
				erpm_interrupt_state=0;
				if(period) erpm_average=FILTER_NEW*(RPM_NUMERATOR/period)+FILTER_OLD*erpm_average;	//Moving average
			}
			else if(REG_TC0_CV0>TIMEOUT_STANDSTILL) erpm_average=0;						//No pulse -> standstill

		//Calculate test bench rpm (TRIGGER_WHEEL_TEETH pulses per revolution)
			if(brpm_interrupt_state)
			{
				uint32_t period=brpm_timer_val;												//Local copy, ISR can overwrite
				brpm_interrupt_state=0;
				if(period) brpm_average=FILTER_NEW*((RPM_NUMERATOR/period)/TRIGGER_WHEEL_TEETH)+FILTER_OLD*brpm_average;	//Moving average
			}
			else if(REG_TC0_CV1>TIMEOUT_STANDSTILL) brpm_average=0;						//No pulse -> standstill

	}//end while

}//end main



void TC0_Handler(void)				//Engine RPM Interrupt
{
	erpm_timer_val=REG_TC0_RA0;		//Store register A (period)
	erpm_interrupt_state=1;			//New reading
	REG_TC0_SR0;					//Read status register -> clear interrupt flags
}

void TC1_Handler(void)				//Bench RPM Interrupt
{
	brpm_timer_val=REG_TC0_RA1;		//Store register A (period)
	brpm_interrupt_state=1;			//New reading
	REG_TC0_SR1;					//Read status register -> clear interrupt flags
}

void TC2_Handler(void)				//Periodic Interrupt every 500µs
{
	if(tft_timer) tft_timer--;
	if(rpm_timer) rpm_timer--;
	if(ndiff_timer) ndiff_timer--;
	if(switch_timer) switch_timer--;

	REG_TC0_SR2;					//Read status register -> clear interrupt flags
}



/*
---Short description of some peculiarities of the µC---

###- TIMER:
The SAM3X8E has 9 timers (counters) in total. There are 3 modules with 3 timers (channels) each.
Addressing registers:

Module ID (0,1,2) is replaced by |M|
Timer ID (0,1,2) is replaced by |T|

Example: REG_TC|M|_CCR|T|

The datasheet is not consistent: for the Interrupt_Handler and for interrupt requests
(and for the peripheral IDs in the PMC) the timers are numbered from 0 to 8:

TC0: Module 0, Timer 0
TC1: Module 0, Timer 1
TC2: Module 0, Timer 2
---------------------
TC3: Module 1, Timer 0
TC4: Module 1, Timer 1
TC5: Module 1, Timer 2
---------------------
TC6: Module 2, Timer 0
TC7: Module 2, Timer 1
TC8: Module 2, Timer 2

Example: in all three cases module 1, timer 2 (TC5) is addressed:
1) REG_TC1_CCR2
2) NVIC_EnableIRQ(TC5_IRQn);
3) void TC5_Handler(void) { //CODE }

###- Interrupts:
The SAM3X8E does not reset its interrupt flags to 0 automatically after execution.
This has to be done by hand by simply reading the status register TC_SRx.
This code line has to be added to the interrupt routine (for a timer interrupt):
REG_TCx_SRx;
The same applies to PIO registers (PIO_ISR) for a port interrupt.
Otherwise the interrupt routine is executed again immediately after it ends (endless loop).

###- Write-only registers:
PIO_PER/PDR/OER/ODR/SODR/CODR/PUER/PUDR/IFER, TC_CCR, TC_IER and PMC_PCER0 are write-only.
Only the bits written as 1 take effect, the others stay unchanged -> always write with =, not with |=.
*/

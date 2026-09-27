// SPDX-FileCopyrightText: Johannes Stockhammer
// SPDX-License-Identifier: Apache-2.0
/*
 * TFT_Main_Screen.ino
 *
 *  Author: Johannes Stockhammer
 *
 * Main screen on the TFT: ratio, bar engine rpm (0..16000 rpm) and bar Ndiff (0..100 rpm).
 * The bars are drawn as outlines every 100ms and painted over in white again.
 */

//Defines
	//Layout (pixels, display 480x320 in landscape)
		#define TFT_BAR_X				10			//Left edge of the bars
		#define TFT_BAR_HEIGHT			30			//Height of the bars
		#define TFT_BAR_MAX				400			//Full bar length
		#define TFT_BAR_ERPM_Y			120			//Bar engine rpm
		#define TFT_BAR_NDIFF_Y			220			//Bar Ndiff
		#define TFT_SCALE_ERPM_Y		100			//Labels engine rpm
		#define TFT_SCALE_NDIFF_Y		200			//Labels Ndiff
		#define TFT_SCALE_ERPM_STEP		25			//Pixels per 1000 rpm
		#define TFT_SCALE_NDIFF_STEP	40			//Pixels per 10 rpm

	//Scales
		#define ERPM_SCALE_MAX			16000		//Engine rpm at full bar
		#define NDIFF_SCALE_MAX			100			//Ndiff at full bar

	//States main screen
		#define TFT_DRAW				0			//Draw background and labels
		#define TFT_BAR					1			//Draw bars
		#define TFT_BAR_CLEAR			2			//Clear bars after 100ms

void TFT_Main_Screen()
{
	uint8_t i;
	uint16_t x;

	if(tft_lastwindow!=TFT_MAINWINDOW) tft_val=TFT_DRAW;			//Other screen was active -> redraw

	switch(tft_val)
	{
		case TFT_DRAW:			tft.fillScreen(HX8357_WHITE);
								tft.setRotation(3);
								tft.setTextColor(HX8357_BLACK);
								tft.setTextSize(2);
								tft.setCursor(10,30);
								tft.print("Ratio:");
								tft.print(rpm_ratio);

								//Bar Engine RPM, scale 0..16 (x1000 rpm)
								tft.setCursor(10,70);
								tft.print("Engine RPM:");
								for(i=0;i<=16;i++)
								{
									x=5+i*TFT_SCALE_ERPM_STEP;				//5, 30, 55, ... 405
									if( (i>10)&&(i%2) )						//11, 13, 15: only a tick mark (space)
									{
										tft.setCursor(x,TFT_SCALE_ERPM_Y);
										tft.print("|");
									}
									else
									{
										if(i>=10) x-=5;						//Two-digit number a bit to the left
										tft.setCursor(x,TFT_SCALE_ERPM_Y);
										tft.print(i);
									}
								}

								//Bar Ndiff RPM, scale 0..100 rpm
								tft.setCursor(10,170);
								tft.print("Ndiff:");
								for(i=0;i<=10;i++)
								{
									x=i*TFT_SCALE_NDIFF_STEP;				//0, 40, 80, ... 400
									if(i==0) x=5;							//"0" not at the edge
									if(i==10) x=395;						//"100" three digits, a bit to the left
									tft.setCursor(x,TFT_SCALE_NDIFF_Y);
									tft.print(i*10);
								}

								//Author, SW version
								tft.setTextSize(1);
								tft.setCursor(10,300);
								tft.print("Johannes Stockhammer, SW:");
								tft.print(SOFTWARE_VERSION);

								tft_lastwindow=TFT_MAINWINDOW;
								tft_val=TFT_BAR;
								break;

		case TFT_BAR:			if(erpm_average>ERPM_SCALE_MAX) tft_x1=TFT_BAR_MAX;				//Limit to full length
								else tft_x1=uint32_t(erpm_average)*TFT_BAR_MAX/ERPM_SCALE_MAX;	//16000 rpm = 400px
								if(rpm_ndiff>NDIFF_SCALE_MAX) tft_x2=TFT_BAR_MAX;				//Limit to full length
								else if(rpm_ndiff<0) tft_x2=0;									//Do not show negative difference
								else tft_x2=rpm_ndiff*TFT_BAR_MAX/NDIFF_SCALE_MAX;				//100 rpm = 400px
								tft_timer=TIME_100ms;											//Leave bars for 100ms
								tft.drawRect(TFT_BAR_X,TFT_BAR_NDIFF_Y,tft_x2,TFT_BAR_HEIGHT,HX8357_BLACK);
								tft.drawRect(TFT_BAR_X,TFT_BAR_ERPM_Y,tft_x1,TFT_BAR_HEIGHT,HX8357_BLACK);
								tft_val=TFT_BAR_CLEAR;
								break;

		case TFT_BAR_CLEAR:		if(!tft_timer)														//Time expired
								{
									tft.drawRect(TFT_BAR_X,TFT_BAR_NDIFF_Y,tft_x2,TFT_BAR_HEIGHT,HX8357_WHITE);	//Paint over in white
									tft.drawRect(TFT_BAR_X,TFT_BAR_ERPM_Y,tft_x1,TFT_BAR_HEIGHT,HX8357_WHITE);
									tft_val=TFT_BAR;
								}
								break;
	}//end switch tft_val
}

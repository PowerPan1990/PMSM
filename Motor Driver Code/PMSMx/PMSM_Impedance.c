/*
 * The MIT License (MIT)
 * 
 * Copyright (c) 2013 Pavlo Milo Manovi
 * 
 * Permission is hereby granted, free of charge, to any person obtaining a copy
 * of this software and associated documentation files (the "Software"), to deal
 * in the Software without restriction, including without limitation the rights
 * to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
 * copies of the Software, and to permit persons to whom the Software is
 * furnished to do so, subject to the following conditions:
 * 
 * The above copyright notice and this permission notice shall be included in
 * all copies or substantial portions of the Software.
 * 
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
 * AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 * LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
 * OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN
 * THE SOFTWARE.
 */

/**
 * @file	PMSM.c
 * @author 	Pavlo Manovi
 * @date 	July, 2013
 * @brief 	This library provides SVPWM with position control with an LQG
 *
 * This library provides implementation of a 3rd order LQG controller for a PMSM motor with
 * space vector modulated sinusoidal pulse width modulation control of a permenant magnet
 * synchronous motor.
 *
 * The LQG controller has been characterized in closed loop with an ARX model using a noise
 * model to uncorrelate the characterized input from noise.  Supporting MATLAB code can be
 * found in the same repository that this code was found in.
 */

#include "PMSMBoard.h"

#if defined (CHARACTERIZE_POSITION) || defined (CHARACTERIZE_VELOCITY)
//Something here
#else
#if defined IMPEDANCE
#include <xc.h>
#include <math.h>
#include <stdlib.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include "PMSM_Position.h"
#include "DMA_Transfer.h"
#include "cordic.h"
#include <qei32.h>
#include <uart.h>

#include "../CAN Testing/canFiles/motor_can.h"

#ifndef CHARACTERIZE
#include "TrigData.h"

#define SQRT_3_2 0.86602540378
#define SQRT_3 1.732050807568877
//#define SPOOL_SCALING_FACTOR 565.486683301
#define TWO_PI 6.283185307
#define SPOOL_RADIUS_MM 15
#define LOOP_TIME_S 0.000333
#define SPOOL_CIRCUMFERENCE_MM (TWO_PI*SPOOL_RADIUS_MM)
#define	SPOOL_SCALING_FACTOR (SPOOL_CIRCUMFERENCE_MM)/LOOP_TIME_S //used full pi instead of 3.14
#define PULSES_PER_REVOLUTION 223232

#define TRANS QEI1STATbits.IDXIRQ
#define POS QEI1STATbits.PCHEQIRQ
#define NEG QEI1STATbits.PCLEQIRQ
#define ZERO QEI1STATbits.POSOVIRQ

typedef struct {
	float Vr1;
	float Vr2;
	float Vr3;
} InvClarkOut;

typedef struct {
	float Va;
	float Vb;
} InvParkOut;

typedef struct {
	uint8_t sector;
	uint16_t T0;
	uint16_t Ta;
	uint16_t Tb;
	float Va;
	float Vb;
} TimesOut;

static float theta;
static float d_u;
static float y;
static float cableVelocity;
static float u = 0;

static int32_t indexCount = 0;
static int32_t runningPositionCount = 0;
static int32_t intermediatePosition = 0;
static int32_t lastRunningPostionCount = 0;

/**
 * @brief Linear Quadradic State Estimation
 *
 * All the state esimates in the Gaussian Estimator are visible here.
 */

static float x_hat[3][1] = {
	{0},
	{0},
	{0},
};

static float x_dummy[3][1] = {
	{0},
	{0},
	{0},
};

static float K_reg[3][3] = {
	{0.167525299197710, 0.853873229166799, -0.054110207360082},
	{-0.557698239898382, 0.248028266562591, 0.116156944634215},
	{0.235670999753073, 0.436664074501789, -0.649680555994633}

};

static float L[3][1] = {
	{ -0.011927622324162},
	{0.464575420462811},
	{0.330672683026544}
};

static float K[1][3] = {
	{0.310247462375861, -0.093017922302088, -0.789859907625239}
};

void SpaceVectorModulation(TimesOut sv);
InvClarkOut InverseClarke(InvParkOut pP);
InvParkOut InversePark(float Vd, float Vq, int16_t position);
TimesOut SVPWMTimeCalc(InvParkOut pP);

/**
 * @brief PMSM initialization call. Aligns rotor and sets QEI offset.
 * @param *information a pointer to the MotorInfo struct that will be updated.
 * @return Returns 1 if successful, returns 0 otherwise.
 */
uint8_t PMSM_Init(MotorInfo *information)
{
	static uint32_t theta1;
	uint32_t i;
	uint32_t j;
	qeiCounter w;

	w.l = 0;
	theta1 = 0;

	for (i = 0; i < 2048; i++) {
		SpaceVectorModulation(SVPWMTimeCalc(InversePark(0.4, 0, theta1)));
		for (j = 0; j < 400; j++) {
			Nop();
		}
		theta1 -= 1;
	}

	for (i = 2048; i > 1; i--) {
		SpaceVectorModulation(SVPWMTimeCalc(InversePark(0.4, 0, theta1)));
		for (j = 0; j < 400; j++) {
			Nop();
		}
		theta1 -= 1;
	}

	SpaceVectorModulation(SVPWMTimeCalc(InversePark(0.7, 0, 0)));
	while (j < 1400000) {
		Nop();
		j++;
	}

	Write32bitQEI1IndexCounter(&w);
	Write32bitQEI1PositionCounter(&w);
	runningPositionCount = 0;

	SpaceVectorModulation(SVPWMTimeCalc(InversePark(0, 0, 0)));
}

/**
 * @brief Sets the commanded position of the motor.
 */
void SetTension(float tension)
{
	theta = tension;
}

/**
 * @brief Calculates last known cable length and returns it.
 * @return Cable length in mm.
 */
int32_t GetCableLength(void)
{
	return((runningPositionCount / PULSES_PER_REVOLUTION) *
		SPOOL_CIRCUMFERENCE_MM);
}

/**
 * @brief Returns last known cable velocity in mm/s.
 * @return Cable velocity in mm/S.
 */
int32_t GetCableVelocity(void)
{
	/**
	 * Cable Velocity: ((2 * Pi * Radius of Spool) * Delta_Rotations) / Ts_Period
	 * Radius of Spool: 30 mm
	 * Ts_Period: 333 uS
	 * Delta_Rotations: runningCount - lastCount
	 *
	 * All of these values combine to make SPOOL_SCALING_FACTOR
	 */
	cableVelocity = ((runningPositionCount - lastRunningPostionCount) /
		PULSES_PER_REVOLUTION) * SPOOL_SCALING_FACTOR;

	return((int32_t) cableVelocity);
}

void PMSM_Update_Tension(void)
{
        indexCount = Read32bitQEI1PositionCounter();
	int32_t intermediatePosition;

	intermediatePosition = (runningPositionCount + indexCount);

//	y = theta - ((float) (int32_t) (intermediatePosition) * 0.0030679616); //Scaling it back into radians.

//	INTEGER16 To = 1;	// Tension Offset
	float K = 0.01;	// Length Gain Value
	float B = 0.001;	// Velocity Gain Value
	float lo = 0;	// Length Offset
	float vo = 0;	// Velocity Offset

        float length = GetCableLength();
        float velocity = GetCableVelocity();

        theta = theta / 1000.00;

	u = (theta + K*(length - lo) + B*(velocity - vo));

//        u = y * 0.03;


	//SATURATION HERE...  IF YOU REALLY NEED MORE JUICE...  UP THIS TO 1 and -1
	if (u > .7) {
		u = .7;
	} else if (u < -.7) {
		u = -.7;
	}

	CO(state_Current_Position) = (int32_t) ((float) runningPositionCount * 0.02814643647496589);

	if (u > 0) {
		//Commutation phase offset
		indexCount += 512; // - rotorOffset; //Phase offset of 90 degrees.
		d_u = u;
	} else {
		indexCount += -512; // - rotorOffset; //Phase offset of 90 degrees.
		d_u = -u;
	}

        // Added these two lines to make the new position controller work.
        // Borrowed from PMSM_Characterize.c
        indexCount = (-indexCount + 2048) % 2048;
	SpaceVectorModulation(SVPWMTimeCalc(InversePark(d_u, 0, indexCount)));
}

void PMSM_Update_Commutation(void)
{
	indexCount = Read32bitQEI1PositionCounter();
	int32_t intermediatePosition;
	intermediatePosition = (runningPositionCount + indexCount);

	if (u > 0) {
		//Commutation phase offset
		indexCount += 512; // - rotorOffset; //Phase offset of 90 degrees.
		d_u = u;
	} else {
		indexCount += -512; // - rotorOffset; //Phase offset of 90 degrees.
		d_u = -u;
	}

	indexCount = (-indexCount + 2048) % 2048;
	SpaceVectorModulation(SVPWMTimeCalc(InversePark(d_u, 0, indexCount)));
}

/****************************   Private Stuff   *******************************/

void SpaceVectorModulation(TimesOut sv)
{
	switch (sv.sector) {
	case 1:
		GH_A_DC = ((uint16_t) PHASE1 * (.5 - .375 * sv.Vb - .649519 * sv.Va)) - 25;
		GH_B_DC = ((uint16_t) PHASE1 * (.5 + .375 * sv.Vb - .216506 * sv.Va)) - 25;
		GH_C_DC = ((uint16_t) PHASE1 * (.5 - .375 * sv.Vb + .216506 * sv.Va)) - 25;
		break;
	case 2:
		GH_A_DC = ((uint16_t) PHASE1 * (.5 - .433013 * sv.Va)) - 25;
		GH_B_DC = ((uint16_t) PHASE1 * (.5 + .75 * sv.Vb)) - 25;
		GH_C_DC = ((uint16_t) PHASE1 * (.5 + .433013 * sv.Va)) - 25;
		break;
	case 3:
		GH_A_DC = ((uint16_t) PHASE1 * (.5 - 0.375 * sv.Vb + .216506 * sv.Va)) - 25;
		GH_B_DC = ((uint16_t) PHASE1 * (.5 + 0.375 * sv.Vb + .216506 * sv.Va)) - 25;
		GH_C_DC = ((uint16_t) PHASE1 * (.5 - 0.375 * sv.Vb + .649519 * sv.Va)) - 25;
		break;
	default:
		break;
	}
}

InvClarkOut InverseClarke(InvParkOut pP)
{
	InvClarkOut returnVal;
	returnVal.Vr1 = pP.Vb;
	returnVal.Vr2 = -.5 * pP.Vb + SQRT_3_2 * pP.Va;
	returnVal.Vr3 = -.5 * pP.Vb - SQRT_3_2 * pP.Va;
	return(returnVal);
}

InvParkOut InversePark(float Vq, float Vd, int16_t position1)
{
	static int position;
	position = position1;
	static int16_t cos_position;
	InvParkOut returnVal;

	float cosine;
	float sine;

	if (position1 <= 0) {
		position = 2048 + (position1 % 2048);
		cos_position = (2048 + ((position1 + 512) % 2048)) % 2048;
	} else {
		position = position1 % 2048;
		cos_position = (position1 + 512) % 2048;
	}

	cosine = TRIG_DATA[cos_position];
	sine = TRIG_DATA[position];

	returnVal.Va = Vd * cosine - Vq * sine;
	returnVal.Vb = Vd * sine + Vq * cosine;
	return(returnVal);
}

TimesOut SVPWMTimeCalc(InvParkOut pP)
{
	TimesOut t;
	t.sector = ((uint8_t) ((.0029296875 * indexCount) + 6)) % 3 + 1;

	t.Va = pP.Va;
	t.Vb = pP.Vb;

	return(t);
}

static int32_t lastCheck;
static int32_t currentCheck;

void __attribute__((__interrupt__, no_auto_psv)) _QEI1Interrupt(void)
{
	currentCheck = Read32bitQEI1PositionCounter();
	lastRunningPostionCount = runningPositionCount;
	runningPositionCount += currentCheck;

	POS = 0;
	NEG = 0;
	ZERO = 0;

	qeiCounter w;

	if (currentCheck > 0) {
		w.l = currentCheck - 2048;
	} else {
		w.l = currentCheck + 2048;
	}

	Write32bitQEI1PositionCounter(&w);
	lastCheck = currentCheck;

	IFS3bits.QEI1IF = 0; /* Clear QEI interrupt flag */
}
#endif
#endif
#endif

/******************************************************************************\

          (c) Copyright Explore Semiconductor, Inc. Limited 2005 
                           ALL RIGHTS RESERVED 
 
--------------------------------------------------------------------------------

 Please review the terms of the license agreement before using this file.
 If you are not an authorized user, please destroy this source code file  
 and notify Explore Semiconductor Inc. immediately that you inadvertently 
 received an unauthorized copy.  

--------------------------------------------------------------------------------

  File        :  HDCP.c 

  Description :  HDCP program for HDMI Transmiter (EP932)

  Codeing     :  Shihken

\******************************************************************************/

// standard Lib

// Our includes
#include "HDCP.h"
#include "EP956_If.h"
#include "DDC_If.h"

#ifndef min
#define min(a,b) (a<b)? a:b
#endif


//
// Global Data
//
unsigned long Temp_SHA_H[5];
unsigned char Temp_HDCP[10];

static int i, j;


//
// System State
//

unsigned int HDCP_TimeCount = 0;
HDCP_STATE HDCP_State = 0;
unsigned char HDCP_Status = 0;
BOOL RI_Check = 0, Fake_HDCP = 0;
unsigned char *pKSVList = NULL, *pBKSV = NULL, *pBCAPS3 = NULL;
unsigned char KSVListNumber = 0;
unsigned char *pRevokeList = NULL;
unsigned char RevokeListSize = 0;
int seed = 0;



//
// Private Functions
//

unsigned char HDCP_validate_RI(void);
unsigned char HDCP_validate_KSV(unsigned char *pKSV);

// Repeater
unsigned char HDCP_compute_SHA_message_digest(unsigned char hdcp_dev_count, unsigned char hdcp_depth);

// SHA
void SHA_Initial(void);
void SHA_Push_Data(PBYTE pData, BYTE Size);
PULONG SHA_Get_SHA_Digest(void);
void SHA_Calculation(unsigned long pSHA_H[5], unsigned long pSHA_W1[16]);

//---------------------------------------------------------------------------------------------------------------------------

void HDCP_Extract_BKSV(unsigned char *pBksv)
{
	pBKSV = pBksv;
}

void HDCP_Extract_BCAPS3(unsigned char *pBCaps3)
{
	pBCAPS3 = pBCaps3;
}

void HDCP_Extract_FIFO(unsigned char *pFIFOList, unsigned char ListNumber)
{	
	EP_DEV_DBG("FIFO Size = %bu\n", ListNumber);
	pKSVList = pFIFOList;
	KSVListNumber = ListNumber;
}

void HDCP_Assign_RKSV_List(unsigned char *pRevocationList, unsigned char Size)
{
	EP_DEV_DBG("RKSV_List Size = %bu\n", Size);
	pRevokeList = pRevocationList;
	RevokeListSize = Size;	   
}

void HDCP_Stop(void)
{
	HDCP_TimeCount = 1000/HDCP_TIMER_PERIOD; // No delay for next startup
	HDCP_Status = 0;
	HDCP_State = 0;
	RI_Check = 0;

	// Disable the HDCP Engine
	HDMI_Tx_HDCP_Disable();
}

void HDCP_Fake(unsigned char Enable)
{
	Fake_HDCP = Enable;
}

unsigned char HDCP_Get_Status(void)
{
	return HDCP_Status;
}

void HDCP_Timer(void) 
{
	++HDCP_TimeCount;
}

void HDCP_Ext_Ri_Trigger(void) 
{
	RI_Check = 1;
}

HDCP_STATE HDCP_Authentication_Task(void)
{
	switch(HDCP_State) {
	// HDCP authentication

		// A0 state -- Wait for Active RX
		//          -- read and validate BKSV for every 1 second.
		case A0_Wait_for_Active_Rx:
 		    
 	    	//if(HDCP_TimeCount >= 1000/HDCP_TIMER_PERIOD) 
			delay_100ms(10);
			{
				//HDCP_TimeCount = 0;
   
				if( Downstream_Rx_read_BKSV(Temp_HDCP) ) {		
					EP_DEV_DBG("Authentication start...\n");
		   			HDCP_State = A1_Exchange_KSVs;
				}
				else {	// HDCP not support
					EP_DEV_DBG("HDCP might not be supported\n");
					Downstream_Rx_write_AINFO(0x00); // Make TE sense the retry to pass ATC HDCP test
					HDCP_Status |= HDCP_ERROR_BKSV;
				}
			}
			break;

		// A1 state -- Exchange KSVs
		//          -- retrieve the random number
		//          -- write AN to RX and TX
		//          -- read and write AKSV, BKSV
		case A1_Exchange_KSVs:

			// Write AINFO
			Downstream_Rx_write_AINFO(0x00);

			// Check Repeater Bit
			Temp_HDCP[0] = Downstream_Rx_BCAPS();
			if(Temp_HDCP[0] & 0x40) {		// REPEATER
				HDMI_Tx_RPTR_Set();
			}
			else {						// NON-REPEATER
				HDMI_Tx_RPTR_Clear();
			}

			// Scramble the seed
			if(!seed) {

				// Wait for V-Sync change
				if(HDMI_Tx_VSYNC()) {
					// Count the High Period
					for(; seed<3000 && (HDMI_Tx_VSYNC() == 1); ++seed) {}
				}
				else {
					// Count the Low Period
					for(; seed<3000 && (HDMI_Tx_VSYNC() == 0); ++seed) {}
				}

				// need to find a replaced function in android
				//srand(seed);
			}

			// Exange AN
			for(Temp_HDCP[8]=0; Temp_HDCP[8]<8; ++Temp_HDCP[8]) {		 	   
				// need to find a replaced function in android
				Temp_HDCP[Temp_HDCP[8]] = 0x00;//rand()%256;
			}
		    HDMI_Tx_write_AN(Temp_HDCP);
		    Downstream_Rx_write_AN(Temp_HDCP);

			// Exange AKSV
		    if(!HDMI_Tx_read_AKSV(Temp_HDCP)) {
			
				if(!Fake_HDCP) {
					HDCP_State = A0_Wait_for_Active_Rx;
					HDCP_Status |= HDCP_ERROR_AKSV;
					break;
				}
				else {
					memset(Temp_HDCP, 0x5A, 5);
				}
			}
		    Downstream_Rx_write_AKSV(Temp_HDCP);

			// Exange BKSV
		    if(!Downstream_Rx_read_BKSV(Temp_HDCP)) {
				HDCP_State = A0_Wait_for_Active_Rx;
				HDCP_Status |= HDCP_ERROR_BKSV;
				break;
			}
		   	HDMI_Tx_write_BKSV(Temp_HDCP);
			if(pBKSV) memcpy(&pBKSV[0], Temp_HDCP, 5);

			// Check Revokation List
			if(!HDCP_validate_KSV(Temp_HDCP)) {
				EP_DEV_DBG("Err: BKSV is revoked\n");
				HDCP_State = A0_Wait_for_Active_Rx;	
				break;
			}
		
		    HDCP_State = A2_Computations;
			//HDCP_TimeCount = 0;
			break;

		// A2 state -- Computations
		//          -- Wait 150ms for R0 update (min 100ms)
		case A2_Computations:

		 	//if(HDCP_TimeCount >= 150/HDCP_TIMER_PERIOD) 
			delay_100ms(1);
			delay_1ms(50);
			{
				if(HDMI_Tx_RI_RDY()) {
					//HDCP_TimeCount = 0;
					HDCP_State = A3_Validate_Receiver;
				}
			}
			break;

		// A3 state -- Validate Receiver
		//          -- read and compare R0 from TX and RX
  		//          -- allow IIC traffic or R0 compare error in 200ms
		case A3_Validate_Receiver:
		{
			static BYTE HDCP_RI_Count = 0;
			if(!HDCP_validate_RI()) {
	 	    	//if(HDCP_TimeCount >= 200/HDCP_TIMER_PERIOD) 
				if(HDCP_RI_Count++ >= 200 / 55)
				{
					//HDCP_TimeCount = 0;
					HDCP_RI_Count = 0;
					
					EP_DEV_DBG("Err: R0 check failed\n");

					HDCP_State = A0_Wait_for_Active_Rx;	
					HDCP_Status |= HDCP_ERROR_R0;
				}
			}
			else {
				HDCP_TimeCount = 0;
				HDCP_State = A6_Test_for_Repeater;
			}
			break;
		}

		// A4 state -- Authenticated
		//          -- Disable mute
		case A4_Authenticated:
					
			// Start the HDCP Engine
			if(!Fake_HDCP) HDMI_Tx_HDCP_Enable(); 
	
			// Disable mute for transmiter video 
			HDMI_Tx_Mute_Disable();

			EP_DEV_DBG("Authenticated\n");			

			HDCP_State = A5_Link_Integrity_Check;
			break;

		// A5 state -- Link Integrity Check every second
		//          -- HDCP Engine must be started
  		//          -- read and compare RI from RX and TX
		case A5_Link_Integrity_Check:

			if(RI_Check) {
				RI_Check = 0;

				if(!HDCP_validate_RI()) {
					if(!HDCP_validate_RI()) {

						// Enable mute for transmiter video and audio
						HDMI_Tx_Mute_Enable();

						// Disable the HDCP Engine
						HDMI_Tx_HDCP_Disable(); 

						EP_DEV_DBG("Err: Ri check failed\n");

						HDCP_State = A0_Wait_for_Active_Rx;
						HDCP_Status |= HDCP_ERROR_Ri;
					}
				}
			}

/* Alternative method to check Ri
 	    	if(HDCP_TimeCount >= 1500/HDCP_TIMER_PERIOD) {	// Wait for 1.5 second
				HDCP_TimeCount = 0;

				if(!HDCP_validate_RI()) {

					if(RI_Check) { // RI_Failed_Two

						// Enable mute for transmiter video and audio
						HDMI_Tx_Mute_Enable();

						// Disable the HDCP Engine
						HDMI_Tx_HDCP_Disable(); 

						EP_DEV_DBG("Err: Ri check failed\n");

						HDCP_State = A0_Wait_for_Active_Rx;
						HDCP_Status |= HDCP_ERROR_Ri;
					}
					else {
						RI_Check = 1;
						HDCP_TimeCount = 1500/HDCP_TIMER_PERIOD;
					}
				}
				else {
					RI_Check = 0;
				}
			}
*/
			break;

		// A6 state -- Test For Repeater
		//          -- REPEATER     : Enter the WAIT_RX_RDY state;
		//          -- NON-REPEATER : Enter the AUTHENTICATED state
		case A6_Test_for_Repeater:   

			Temp_HDCP[0] = Downstream_Rx_BCAPS();
			if(pBCAPS3) pBCAPS3[0] = Temp_HDCP[0];

			if (Temp_HDCP[0] & 0x40) {	// REPEATER	
				HDCP_State = A8_Wait_for_READY;
			}
			else {						// NON-REPEATER
				HDCP_State = A4_Authenticated;
			}
			break;

		// A8 state -- Wait for READY
		//          -- read BCAPS and check READY bit continuously
		//          -- time out while 5-second period exceeds
		case A8_Wait_for_READY:   

			Temp_HDCP[0] = Downstream_Rx_BCAPS();
			if(pBCAPS3) pBCAPS3[0] = Temp_HDCP[0];

			if (Temp_HDCP[0] & 0x20) {
				//HDCP_TimeCount = 0;
				HDCP_State = A9_Read_KSV_List;
			}
			else {
				static int HDCP_Ready_Count = 0;
				//if(HDCP_TimeCount >= 5000/HDCP_TIMER_PERIOD) 
				if(HDCP_Ready_Count++ >= 5000 / 55)
				{
					//HDCP_TimeCount = 0;
					HDCP_Ready_Count = 0;

					EP_DEV_DBG("Err: Repeater check READY bit time-out\n");

					HDCP_State = A0_Wait_for_Active_Rx;	
					HDCP_Status |= HDCP_ERROR_RepeaterRdy;
				}
			}
			break;

		// A9 state -- Read KSV List
		//          -- compute and validate SHA-1 values
		case A9_Read_KSV_List:
			
			Downstream_Rx_read_BSTATUS(Temp_HDCP);
			if(pBCAPS3) memcpy(&pBCAPS3[1], Temp_HDCP, 2);

			if(HDCP_compute_SHA_message_digest(Temp_HDCP[0], Temp_HDCP[1])) {
				HDCP_State = A4_Authenticated;
				break;
			}

			HDCP_Status |= HDCP_ERROR_RepeaterSHA;


			EP_DEV_DBG("Err: Repeater HDCP SHA check failed\n");

			HDCP_State = A0_Wait_for_Active_Rx;	
			break;
	}

	return HDCP_State;
}

//----------------------------------------------------------------------------------------------------------------------

unsigned char HDCP_validate_RI(void)
{
	unsigned short Temp_RI_Tx, Temp_RI_Rx;
	if(!HDMI_Tx_read_RI((unsigned char *)&Temp_RI_Tx)) return 0;		// Read form Tx is fast, do it first
	if(!Downstream_Rx_read_RI((unsigned char *)&Temp_RI_Rx)) return 0;	// Read form Rx is slow, do it second
//	if(Temp_RI_Tx != Temp_RI_Rx) EP_DEV_DBG("RI_Tx=0x%0.4X, RI_Rx=0x%0.4X\n", (int)Temp_RI_Tx, (int)Temp_RI_Rx);
	if(Fake_HDCP) return 1;
	return (Temp_RI_Tx == Temp_RI_Rx);
}

// 0: invalide KSV found, 1: OK
unsigned char HDCP_validate_KSV(unsigned char *pKSV)
{
	if(pRevokeList) {
		unsigned char i;

//		EP_DEV_DBG("Revoke KSV\n");
		for(i=0; i<RevokeListSize; i+=5) {

//			EP_DEV_DBG("%02X %02X %02X %02X %02X", pRevokeList[i+0], pRevokeList[i+1], pRevokeList[i+2], pRevokeList[i+3], pRevokeList[i+4] );
			if(memcmp(pRevokeList+i, pKSV, 5) == 0) {
//				EP_DEV_DBG("- Match\n");
				return 0;
			}
//			EP_DEV_DBG("- Not Match\n");
		}
	}
	return 1;
}

//--------------------------------------------------------------------------------------------------

unsigned char HDCP_compute_SHA_message_digest(unsigned char hdcp_dev_count, unsigned char hdcp_depth) 
{
	PULONG SHA_H;
	BOOL Result;

	EP_DEV_DBG("hdcp_dev_count=0x%02X\n", hdcp_dev_count );
	EP_DEV_DBG("hdcp_depth=0x%02X\n", hdcp_depth );

	//////////////////////////////////////////////////////////////////////////////////////////////
	// Calculate SHA Value
	//
	SHA_Initial();

	//
	// Step 1
	// Push all KSV FIFO to SHA caculation
	//

	// Read KSV (5 byte) one by one and check the revocation list
	for(i=0; i<(hdcp_dev_count & 0x7F); ++i) {

		// Get KSV from FIFO
		if(!Downstream_Rx_read_KSV_FIFO(Temp_HDCP, i, (hdcp_dev_count & 0x7F))) {
			return 0;
		}

		// Check Revokation List
		if(!HDCP_validate_KSV(Temp_HDCP)) {
			EP_DEV_DBG("Err: KSV is revoked\n");		
			return 0;
		}
		// Push KSV to the Downstream KSV List for Repeater usage.	
		if(pKSVList && KSVListNumber) {	
			if(i < KSVListNumber) memcpy(pKSVList+(i*5), Temp_HDCP, 5);
		}

		// Push KSV to the SHA block buffer (Total 5 bytes)
		SHA_Push_Data(Temp_HDCP, 5);
	}


	//
	// Step 2
	// Push BSTATUS, M0, and EOF to SHA caculation
	//

	// Get the BSTATUS, M0, and EOF
	Temp_HDCP[0] = hdcp_dev_count;	// Temp_HDCP[0]   = BStatus, LSB
	Temp_HDCP[1] = hdcp_depth;		// Temp_HDCP[1]   = BStatus, MSB
	HDMI_Tx_read_M0(Temp_HDCP+2);	// Temp_HDCP[2:9] = Read M0 from TX

	// Push the BSTATUS, and M0 to the SHA block buffer (Total 10 bytes)
	SHA_Push_Data(Temp_HDCP, 10);

	//
	// Step 3
	// Get the final SHA Digit
	//

	SHA_H = SHA_Get_SHA_Digest();

	//
	// SHA complete
	//////////////////////////////////////////////////////////////////////////////////////////////



	//////////////////////////////////////////////////////////////////////////////////////////////
	// Compare the SHA value and Check Max Device and Depth
	//

	// read RX SHA value
	Downstream_Rx_read_SHA1_HASH((unsigned char*)Temp_SHA_H); 
	EP_DEV_DBG("Rx_SHA_H: ");  
#ifdef DBG
	for(i=0; i<5; i++) {		
		EP_DEV_DBG("0x%08lX ", Temp_SHA_H[i]);
	}
	EP_DEV_DBG("\n");
#endif

	// compare the TX/RX SHA value
	if( memcmp(SHA_H, Temp_SHA_H, 20) ) {
		EP_DEV_DBG("Err: SHA Digit Unmatch\n");
  		Result = 0;
	}
	else {
		EP_DEV_DBG("SHA Digit Match\n");
		Result = 1;
	}

	// Check Max Device and Depth exceed

	// Check Depth
	if( (hdcp_depth & 0x0F) >= 0x07 ) { // Need to Mask the other bits
		// Already exceed then copy
		// Almost exceed then set exceed flag
//		Downstream_Device_Depth = (hdcp_depth & 0x0F) | 0x08; // Copy and Set exceed flag

		EP_DEV_DBG("Err: Max Cascade exceeded\n");
		HDCP_Status |= HDCP_ERROR_RepeaterMax;
		Result = 0;
	}
	else {
//		Downstream_Device_Depth = max(hdcp_depth & 0x0F, Downstream_Device_Depth);
	}

	// Check Device Count
	if( hdcp_dev_count >= (KSVListNumber+1) ) {
		// Already exceed then copy
		// Almost exceed then set Exceed flag
//		if(hdcp_dev_count < (KSVListNumber+1)) ++hdcp_dev_count;
//		Downstream_Device_Count = hdcp_dev_count |= 0x80; // Copy and Set exceed flag
		
		EP_DEV_DBG("Err: Max Devs exceeded\n");
		HDCP_Status |= HDCP_ERROR_RepeaterMax;
		Result = 0;
	}

//	if(pKSVList) {
//		EP_DEV_DBG("Downstream_Device_Count=0x%X\n", Downstream_Device_Count );
//		EP_DEV_DBG("Downstream_Device_Depth=0x%X\n", Downstream_Device_Depth );
//	}

	return Result;

	//
	// Return the compared result
	//////////////////////////////////////////////////////////////////////////////////////////////

}

//--------------------------------------------------------------------------------------------------
// SHA Implementation
//--------------------------------------------------------------------------------------------------

ULONG SHA_H[5];
BYTE SHA_Block[64];	 	// 16*4
BYTE SHA_Block_Index;
BYTE CopySize;
WORD SHA_Length;

void SHA_Initial(void)
{
	//////////////////////////////////////////////////////////////////////////////////////////////
	// Calculate SHA Value
	//

	// initial the SHA variables
	SHA_H[0] = 0x67452301;
	SHA_H[1] = 0xEFCDAB89;
	SHA_H[2] = 0x98BADCFE;
	SHA_H[3] = 0x10325476;
	SHA_H[4] = 0xC3D2E1F0;

	// Clean the SHA Block buffer
	memset(SHA_Block, 0, 64);
	SHA_Block_Index = 0;

	SHA_Length = 0;
}

void SHA_Push_Data(PBYTE pData, BYTE Size)
{
	SHA_Length += Size;

	while(Size) {
		// Push Data to the SHA block buffer
		CopySize = min((64-SHA_Block_Index), Size);
		memcpy(SHA_Block+SHA_Block_Index, pData, CopySize);
		SHA_Block_Index += CopySize;
		pData += CopySize;
		Size -= CopySize;

		if(SHA_Block_Index >= 64) { // The SHA block buffer Full

			// Do SHA caculation for this SHA block buffer
			SHA_Calculation(SHA_H, (ULONG*)SHA_Block);
			memset(SHA_Block, 0, 64);				
	
			SHA_Block_Index = 0; // Reset the Index
		}
	}
}

PULONG SHA_Get_SHA_Digest(void)
{
	SHA_Block[SHA_Block_Index++] = 0x80;	// Set EOF

	if((64 - SHA_Block_Index) < 2) {
		// Do SHA caculation for final SHA block
		SHA_Calculation(SHA_H, (ULONG*)SHA_Block);
		memset(SHA_Block, 0, 64);
	}
	SHA_Length *= 8; 
	SHA_Block[62] = (SHA_Length >> 8) & 0xFF; 			// Pad with Length MSB
	SHA_Block[63] = SHA_Length & 0xFF;  				// Pad with Length LSB

	// Do SHA caculation for final SHA block
	SHA_Calculation(SHA_H, (ULONG*)SHA_Block);
	
	// Swap the sequence of SHA_H (The big-endian to little-endian)
	for(i=0; i<20; i+=4) {
	   
		Temp_HDCP[0] = ((PBYTE)SHA_H)[i+0];
		((PBYTE)SHA_H)[i+0] = ((PBYTE)SHA_H)[i+3];
		((PBYTE)SHA_H)[i+3] = Temp_HDCP[0];

		Temp_HDCP[0] = ((PBYTE)SHA_H)[i+1];
		((PBYTE)SHA_H)[i+1] = ((PBYTE)SHA_H)[i+2];
		((PBYTE)SHA_H)[i+2] = Temp_HDCP[0];
	} 

	// Pring SHA
	EP_DEV_DBG("SHA_H:    "); 
	for(i=0; i<5; i++) {
		EP_DEV_DBG("0x%08lX ", SHA_H[i]);
	}
	EP_DEV_DBG("\n"); 

	return SHA_H;
}

void SHA_Calculation(unsigned long pSHA_H[5], unsigned long pSHA_W1[16])
{
	unsigned char i;
	unsigned long TEMP;

	// =========================================================
	//
	// STEP (c) : Let A = H0, B = H1, C = H2, D = H3, E = H4
	//
	Temp_SHA_H[0] = pSHA_H[0];
	Temp_SHA_H[1] = pSHA_H[1];
	Temp_SHA_H[2] = pSHA_H[2];
	Temp_SHA_H[3] = pSHA_H[3];
	Temp_SHA_H[4] = pSHA_H[4];
	//
	// =========================================================    
													
	// =========================================================
	//
	// STEP (d) : FOR t = 0 to 79 DO
	//              1. TEMP = S5(A) + Ft(B,C,D) + E + Wt + Kt
	//              2. E = D; D = C; C = S30(B); B = A; A = TEMP;
	//
	for (i = 0; i <= 79; i++) {
		// Update the Message Word while loop time >= 16
		if (i >= 16) {
			// tword = pSHA_W1[tm03] ^ pSHA_W1[tm08] ^ pSHA_W1[tm14] ^ pSHA_W1[tm16];                   
			TEMP = pSHA_W1[(i + 13) % 16] ^ pSHA_W1[(i + 8) % 16] ^ pSHA_W1[(i + 2) % 16] ^ pSHA_W1[i % 16];
			pSHA_W1[i % 16] = (TEMP << 1) | (TEMP >> 31);
		}

		// Calculate first equation
		TEMP = pSHA_W1[i % 16];

	    TEMP += ((Temp_SHA_H[0] << 5) | (Temp_SHA_H[0] >> 27));
	
	    if (i <= 19)      TEMP += ((Temp_SHA_H[1] & Temp_SHA_H[2]) | (~Temp_SHA_H[1] & Temp_SHA_H[3])) + 0x5A827999;
	    else if (i <= 39) TEMP += (Temp_SHA_H[1] ^ Temp_SHA_H[2] ^ Temp_SHA_H[3]) + 0x6ED9EBA1;
	    else if (i <= 59) TEMP += ((Temp_SHA_H[1] & Temp_SHA_H[2]) | (Temp_SHA_H[1] & Temp_SHA_H[3]) | (Temp_SHA_H[2] & Temp_SHA_H[3])) + 0x8F1BBCDC;
	    else              TEMP += (Temp_SHA_H[1] ^ Temp_SHA_H[2] ^ Temp_SHA_H[3]) + 0xCA62C1D6;

	    TEMP += Temp_SHA_H[4];

    	// Update the Value A/B/C/D/E
    	Temp_SHA_H[4] = Temp_SHA_H[3];
    	Temp_SHA_H[3] = Temp_SHA_H[2];
    	Temp_SHA_H[2] = ((Temp_SHA_H[1] << 30) | (Temp_SHA_H[1] >> 2));
    	Temp_SHA_H[1] = Temp_SHA_H[0];
    	Temp_SHA_H[0] = TEMP;
	}
	//
	// =========================================================

	// =========================================================
	//
	// STEP (e) : H0 = H0 + A; H1 = H1 + B; H2 = H2 + C; H3 = H3 + D; H4 = H4 + E;
	//
	pSHA_H[0] += Temp_SHA_H[0];
	pSHA_H[1] += Temp_SHA_H[1];
	pSHA_H[2] += Temp_SHA_H[2];
	pSHA_H[3] += Temp_SHA_H[3];
	pSHA_H[4] += Temp_SHA_H[4];
	//
	// =========================================================
}


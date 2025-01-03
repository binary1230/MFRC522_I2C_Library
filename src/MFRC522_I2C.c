/*
* MFRC522.cpp - Library to use ARDUINO RFID MODULE KIT 13.56 MHZ WITH TAGS I2C BY AROZCAN
* MFRC522.cpp - Based on ARDUINO RFID MODULE KIT 13.56 MHZ WITH TAGS SPI Library BY COOQROBOT.
* NOTE: Please also check the comments in MFRC522.h - they provide useful hints and background information.
* Released into the public domain.
* Author: arozcan @ https://github.com/arozcan/MFRC522-I2C-Library
* original fork was for use with Arduino Framework.
*
* This version modified for use on ESP32 only using native ESP-IDF (no arduino) by Dominic Cerquetti
*/

#include <freertos/FreeRTOS.h>
#include <freertos/task.h>

#include "esp_log.h"
#include "driver/i2c.h"
#include "driver/gpio.h"

#include <MFRC522_I2C.h>
#include <rom/gpio.h>
#include <memory.h>

#ifdef ARDUINO
// if you hit this, you're trying to use this with the Arduino framework.
// this fork of the library is ONLY intended for use with ESP-IDF natively (no arduino involved)
// see other forks for the arduino-friendly version
#error "This fork of this library is NOT compatible with non-ESP-IDF native usage. don't use this with Arduino framework"
#endif

typedef struct {
    // I2C address of MFRC chip
    uint16_t _chipAddress;

    // if not GPIO_NUM_NC, we'll pulse this GPIO pin# connected to MFRC522's reset and power down input (Pin 6, NRSTPD, active low)
    int _resetPowerDownPin;

    // set to true to enable more debug logging
    bool _logDebugInfo;

    // set to true if this device has been initialized
    bool _initialized;

    // when performing i2c operations, how long should we block and wait before timing out?
    TickType_t _i2cIoTimeoutTicks;
} MFRC5222;

// TODO: doing this as a global means we can only have one device and there's global state.
//  to support multiple devices, remove g_mfrc and instead pass around "struct MFRC5222* device" to each function in the API
static MFRC5222 g_mfrc = {
        ._chipAddress = 0,
        ._resetPowerDownPin = -1,
        ._logDebugInfo = false,
        ._initialized = false,
        ._i2cIoTimeoutTicks = pdMS_TO_TICKS(1000),
        // ._uid = {}
};

// --------------------------------------------------------------------------------
// BEGIN HACKY FAKE ARDUINO SERIAL PRINTING API WRAPPER
// please don't rely on this for anything important
// do not try and use this with actual arduino code, you will likely be saddened.
// a cleaner/better approach would be to just rip out all serial_printxxx() in this file and replace with native
// ESP_LOGxx() statements. to optimize for mergability though, we'll leave the Serial wrapper code here in case we
// want to merge changes back and forth with upstream libs.
// -------------------------------------------------------------------------------
enum Format {
    DEC=01,
    HEX=02,
};

// NOTE: can't directly use the most common ESP_LOGxxx() here
//       because print() expects us to not put a newline in there
static void serial_print(const char *msg)
{
    // no newline
    printf("%s", msg);
}

static void serial_print_f(int i, enum Format format)
{
    // no newline
    printf(format == HEX ? "%x" : "%d", i);
}

static void serial_println(const char *msg)
{
    // add newline
    serial_print(msg);
    serial_print("\n");
}

static void serial_println_f(int i, enum Format format)
{
    // add newline
    serial_print_f(i, format);
    serial_print("\n");
}
// ----------------------------------------
// END FAKE ARDUINO WRAPPER STUFF
// ----------------------------------------

/////////////////////////////////////////////////////////////////////////////////////
// Functions for setting up the Arduino
/////////////////////////////////////////////////////////////////////////////////////

bool MFRC522_Init(uint8_t chipAddress, int resetPowerDownPin)
{
    g_mfrc._chipAddress = chipAddress;
    g_mfrc._resetPowerDownPin = resetPowerDownPin; // -1 to skip
    g_mfrc._initialized = true;
    g_mfrc._logDebugInfo = false;
    g_mfrc._i2cIoTimeoutTicks = pdMS_TO_TICKS(1000);

    return true;
}

/////////////////////////////////////////////////////////////////////////////////////
// Basic interface functions for communicating with the MFRC522
/////////////////////////////////////////////////////////////////////////////////////

/**
 * Writes a byte to the specified register in the MFRC522 chip.
 * The interface is described in the datasheet section 8.1.2.
 * Note: this will BLOCK while waiting for the I2C IO
 */
void PCD_WriteRegister(	    uint8_t reg,		///< The register to write to. One of the PCD_Register enums.
                            uint8_t value		///< The value to write.
                        ) {
    // TODO: handle errors, AT ALL.
    i2c_cmd_handle_t cmd = i2c_cmd_link_create();
    i2c_master_start(cmd);
    i2c_master_write_byte(cmd, (g_mfrc._chipAddress << 1) | I2C_MASTER_WRITE, true);
    i2c_master_write_byte(cmd, reg, true);
    i2c_master_write_byte(cmd, value, true);
    i2c_master_stop(cmd);
    /*esp_err_t ret = */ i2c_master_cmd_begin(I2C_NUM_0, cmd, g_mfrc._i2cIoTimeoutTicks);
    i2c_cmd_link_delete(cmd);
} // End PCD_WriteRegister()

/**
 * Writes a number of bytes to the specified register in the MFRC522 chip.
 * The interface is described in the datasheet section 8.1.2.
 */
void PCD_WriteRegisterData(	uint8_t reg,		///< The register to write to. One of the PCD_Register enums.
                            uint8_t count,		///< The number of bytes to write to the register
                            uint8_t *values	    ///< The values to write. Byte array.
								) {
    if (count == 0) {
        return;
    }

    // TODO: must handle errors
    i2c_cmd_handle_t cmd = i2c_cmd_link_create();
    i2c_master_start(cmd);
    i2c_master_write_byte(cmd, (g_mfrc._chipAddress << 1) | I2C_MASTER_WRITE, true);
    i2c_master_write_byte(cmd, reg, true);
    i2c_master_write(cmd, values, count, true);
    i2c_master_stop(cmd);
    i2c_master_cmd_begin(I2C_NUM_0, cmd, g_mfrc._i2cIoTimeoutTicks);
    i2c_cmd_link_delete(cmd);
} // End PCD_WriteRegister()

/**
 * Reads a byte from the specified register in the MFRC522 chip.
 * The interface is described in the datasheet section 8.1.2.
 * Note: this will BLOCK while waiting for the I2C IO
 */
uint8_t PCD_ReadRegister(uint8_t reg	///< The register to read from. One of the PCD_Register enums.
								) {
    // TODO: [way] better error handling

    uint8_t value = 0; // Variable to store the read value

    // Creating I2C command link
    i2c_cmd_handle_t cmd = i2c_cmd_link_create(); // warning: malloc
    i2c_master_start(cmd);

    // Writing the register address we want to read from
    i2c_master_write_byte(cmd, (g_mfrc._chipAddress << 1) | I2C_MASTER_WRITE, true);
    i2c_master_write_byte(cmd, reg, true);
    i2c_master_stop(cmd);
    esp_err_t err = i2c_master_cmd_begin(I2C_NUM_0, cmd, g_mfrc._i2cIoTimeoutTicks); // Sending the command
    i2c_cmd_link_delete(cmd); // Deleting the command link
    if (err != ESP_OK)
        return 0;

    // ----

    // Creating a new command link for reading
    cmd = i2c_cmd_link_create();
    i2c_master_start(cmd);

    // Setting up and sending the device address for reading
    i2c_master_write_byte(cmd, (g_mfrc._chipAddress << 1) | I2C_MASTER_READ, true);

    // Reading the byte from the specified register
    i2c_master_read_byte(cmd, &value, I2C_MASTER_NACK); // NACK for the last byte
    i2c_master_stop(cmd);
    err = i2c_master_cmd_begin(I2C_NUM_0, cmd, g_mfrc._i2cIoTimeoutTicks); // Sending the command
    i2c_cmd_link_delete(cmd);
    if (err != ESP_OK)
        return 0;

    return value;
} // End PCD_ReadRegister()

/**
 * Reads a number of bytes from the specified register in the MFRC522 chip.
 * The interface is described in the datasheet section 8.1.2.
 * Note: this will BLOCK while waiting for the I2C IO
 */
void PCD_ReadRegisterData(  uint8_t reg,		///< The register to read from. One of the PCD_Register enums.
                            uint8_t count,		///< The number of bytes to read
                            uint8_t *values,	///< Byte array to store the values in.
                            uint8_t rxAlign	    ///< Only bit positions rxAlign..7 in values[0] are updated.
                        ) {
    if (count == 0) {
        return;
    }

    *values = 0;

    // TODO: must handle errors

    // Address translation for reading
    uint8_t address = (g_mfrc._chipAddress << 1) | I2C_MASTER_WRITE; // for setting the register pointer

    // Start the I2C write-read transaction
    i2c_cmd_handle_t cmd = i2c_cmd_link_create();
    i2c_master_start(cmd);

    // Send the device address and write bit
    i2c_master_write_byte(cmd, address, true);
    // Send the register address
    i2c_master_write_byte(cmd, reg, true);
    i2c_master_stop(cmd);
    esp_err_t err = i2c_master_cmd_begin(I2C_NUM_0, cmd, g_mfrc._i2cIoTimeoutTicks); // Execute the commands
    i2c_cmd_link_delete(cmd); // Clean up
    if (err != ESP_OK)
        return;

    // Now read from the register
    address = (g_mfrc._chipAddress << 1) | I2C_MASTER_READ; // Switch to read mode
    cmd = i2c_cmd_link_create();
    i2c_master_start(cmd);
    i2c_master_write_byte(cmd, address, true);

    // Read the required number of bytes
    if(count > 1) {
        i2c_master_read(cmd, values, count - 1, I2C_MASTER_ACK); // ACK all but the last byte
    }
    i2c_master_read_byte(cmd, values + count - 1, I2C_MASTER_NACK); // NACK the last byte

    i2c_master_stop(cmd);
    err = i2c_master_cmd_begin(I2C_NUM_0, cmd, g_mfrc._i2cIoTimeoutTicks); // Execute the commands
    i2c_cmd_link_delete(cmd); // Clean up
    if (err != ESP_OK)
        return;

    // If rxAlign is used, adjust the first byte
    if (rxAlign != 0) {
        uint8_t mask = ((1 << (rxAlign + 1)) - 1) << (7 - rxAlign);
        values[0] = (values[0] & ~mask) | (values[0] & mask);
    }
} // End PCD_ReadRegister()

/**
 * Sets the bits given in mask in register reg.
 */
void PCD_SetRegisterBitMask(	uint8_t reg,	///< The register to update. One of the PCD_Register enums.
                                        uint8_t mask	///< The bits to set.
									) {
    uint8_t tmp;
	tmp = PCD_ReadRegister(reg);
	PCD_WriteRegister(reg, tmp | mask);			// set bit mask
} // End PCD_SetRegisterBitMask()

/**
 * Clears the bits given in mask from register reg.
 */
void PCD_ClearRegisterBitMask(	uint8_t reg,	///< The register to update. One of the PCD_Register enums.
                                        uint8_t mask	///< The bits to clear.
									  ) {
    uint8_t tmp;
	tmp = PCD_ReadRegister(reg);
	PCD_WriteRegister(reg, tmp & (~mask));		// clear bit mask
} // End PCD_ClearRegisterBitMask()


/**
 * Use the CRC coprocessor in the MFRC522 to calculate a CRC_A.
 *
 * @return STATUS_OK on success, STATUS_??? otherwise.
 */
uint8_t PCD_CalculateCRC(	uint8_t *data,		///< In: Pointer to the data to transfer to the FIFO for CRC calculation.
                            uint8_t length,	    ///< In: The number of bytes to transfer.
                            uint8_t *result	    ///< Out: Pointer to result buffer. Result is written to result[0..1], low byte first.
					 ) {
	PCD_WriteRegister(CommandReg, PCD_Idle);		// Stop any active command.
	PCD_WriteRegister(DivIrqReg, 0x04);				// Clear the CRCIRq interrupt request bit
	PCD_SetRegisterBitMask(FIFOLevelReg, 0x80);		// FlushBuffer = 1, FIFO initialization
	PCD_WriteRegisterData(FIFODataReg, length, data);	// Write data to the FIFO
	PCD_WriteRegister(CommandReg, PCD_CalcCRC);		// Start the calculation

	// Wait for the CRC calculation to complete. Each iteration of the while-loop takes 17.73�s.
    unsigned int i = 5000;
    uint8_t n;
	while (1) {
		n = PCD_ReadRegister(DivIrqReg);	// DivIrqReg[7..0] bits are: Set2 reserved reserved MfinActIRq reserved CRCIRq reserved reserved
		if (n & 0x04) {						// CRCIRq bit set - calculation done
			break;
		}
		if (--i == 0) {						// The emergency break. We will eventually terminate on this one after 89ms. Communication with the MFRC522 might be down.
			return STATUS_TIMEOUT;
		}
	}
	PCD_WriteRegister(CommandReg, PCD_Idle);		// Stop calculating CRC for new content in the FIFO.

	// Transfer the result from the registers to the result buffer
	result[0] = PCD_ReadRegister(CRCResultRegL);
	result[1] = PCD_ReadRegister(CRCResultRegH);
	return STATUS_OK;
} // End PCD_CalculateCRC()


/////////////////////////////////////////////////////////////////////////////////////
// Functions for manipulating the MFRC522
/////////////////////////////////////////////////////////////////////////////////////

/// NOTE: please customize GPIO initialization to suit your project's needs
/// return false if software reset is still needed, true if we handled it here
bool PCD_HardGpioReset()
{
    // Set the resetPowerDownPin as digital output, do not reset or(typo? "on"?) power down.
    if (g_mfrc._resetPowerDownPin == -1)
        return false;

    gpio_num_t gpio_num = (gpio_num_t)(g_mfrc._resetPowerDownPin);
    gpio_reset_pin(gpio_num);
    gpio_set_direction(gpio_num, GPIO_MODE_OUTPUT);

    // already powered up?
    if (gpio_get_level(gpio_num) != 0)
        return false; // SW reset needed

    //The MFRC522 chip is in power down mode.
    gpio_set_level(gpio_num, 1);        // Exit power down mode. This triggers a hard reset.

    // Section 8.8.2 in the datasheet says the oscillator start-up time is the start up time of
    // the crystal + 37,74�s. Let us be generous: 50ms+
    vTaskDelay(pdMS_TO_TICKS(100));

    // reset success, we don't need to do a SW reset
    return true;
}

/**
 * Initializes the MFRC522 chip.
 */
void PCD_Init()
{
    // Perform a soft reset if necessary
    if (!PCD_HardGpioReset()) {
		PCD_Reset(); // soft reset
	}

	// When communicating with a PICC we need a timeout if something goes wrong.
	// f_timer = 13.56 MHz / (2*TPreScaler+1) where TPreScaler = [TPrescaler_Hi:TPrescaler_Lo].
	// TPrescaler_Hi are the four low bits in TModeReg. TPrescaler_Lo is TPrescalerReg.
	PCD_WriteRegister(TModeReg, 0x80);			// TAuto=1; timer starts automatically at the end of the transmission in all communication modes at all speeds
	PCD_WriteRegister(TPrescalerReg, 0xA9);		// TPreScaler = TModeReg[3..0]:TPrescalerReg, ie 0x0A9 = 169 => f_timer=40kHz, ie a timer period of 25�s.
	PCD_WriteRegister(TReloadRegH, 0x03);		// Reload timer with 0x3E8 = 1000, ie 25ms before timeout.
	PCD_WriteRegister(TReloadRegL, 0xE8);

	PCD_WriteRegister(TxASKReg, 0x40);		// Default 0x00. Force a 100 % ASK modulation independent of the ModGsPReg register setting
	PCD_WriteRegister(ModeReg, 0x3D);		// Default 0x3F. Set the preset value for the CRC coprocessor for the CalcCRC command to 0x6363 (ISO 14443-3 part 6.2.4)

	PCD_AntennaOn();						// Enable the antenna driver pins TX1 and TX2 (they were disabled by the reset)
} // End PCD_Init()

/**
 * Performs a soft reset on the MFRC522 chip and waits for it to be ready again.
 */
void PCD_Reset() {
	PCD_WriteRegister(CommandReg, PCD_SoftReset);	// Issue the SoftReset command.
	// The datasheet does not mention how long the SoftRest command takes to complete.
	// But the MFRC522 might have been in soft power-down mode (triggered by bit 4 of CommandReg)
	// Section 8.8.2 in the datasheet says the oscillator start-up time is the start up time of the crystal + 37.74ms. Let us be generous: 50ms.
    vTaskDelay(pdMS_TO_TICKS(50));
	// Wait for the PowerDown bit in CommandReg to be cleared
	while (PCD_ReadRegister(CommandReg) & (1<<4)) {
		serial_println("PCD Still restarting after SoftReset");
		// PCD still restarting - unlikely after waiting 50ms, but better safe than sorry.
	}
} // End PCD_Reset()

void PCD_SetMaxInductance()
{
	// experimental, not sure this actually does anything useful.
	// purports to increase the conductance of the TX pins and
	// potentially increase the range of scans (uses/drives more power)
	PCD_WriteRegister(CWGsPReg, 0b111111);
	PCD_WriteRegister(ModGsPReg, 0b111111);
	PCD_WriteRegister(GsNReg, 0b11111111);
}

/**
 * Turns the antenna on by enabling pins TX1 and TX2.
 * After a reset these pins are disabled.
 */
void PCD_AntennaOn() {
    uint8_t value = PCD_ReadRegister(TxControlReg);
	if ((value & 0x03) != 0x03) {
		PCD_WriteRegister(TxControlReg, value | 0x03);
	}
} // End PCD_AntennaOn()

/**
 * Turns the antenna off by disabling pins TX1 and TX2.
 */
void PCD_AntennaOff() {
	PCD_ClearRegisterBitMask(TxControlReg, 0x03);
} // End PCD_AntennaOff()

/**
 * Get the current MFRC522 Receiver Gain (RxGain[2:0]) value.
 * See 9.3.3.6 / table 98 in http://www.nxp.com/documents/data_sheet/MFRC522.pdf
 * NOTE: Return value scrubbed with (0x07<<4)=01110000b as RCFfgReg may use reserved bits.
 *
 * @return Value of the RxGain, scrubbed to the 3 bits used.
 */
uint8_t PCD_GetAntennaGain() {
	return PCD_ReadRegister(RFCfgReg) & (0x07<<4);
} // End PCD_GetAntennaGain()

/**
 * Set the MFRC522 Receiver Gain (RxGain) to value specified by given mask.
 * See 9.3.3.6 / table 98 in http://www.nxp.com/documents/data_sheet/MFRC522.pdf
 * NOTE: Given mask is scrubbed with (0x07<<4)=01110000b as RCFfgReg may use reserved bits.
 */
void PCD_SetAntennaGain(uint8_t mask) {
	if (PCD_GetAntennaGain() != mask) {						// only bother if there is a change
		PCD_ClearRegisterBitMask(RFCfgReg, (0x07<<4));		// clear needed to allow 000 pattern
		PCD_SetRegisterBitMask(RFCfgReg, mask & (0x07<<4));	// only set RxGain[2:0] bits
	}
} // End PCD_SetAntennaGain()

/**
 * Performs a self-test of the MFRC522
 * See 16.1.1 in http://www.nxp.com/documents/data_sheet/MFRC522.pdf
 *
 * @return Whether or not the test passed.
 */
bool PCD_PerformSelfTest()
{
    #if MFRC_INCLUDE_SELFTEST != 1
    // main reason to disable is simply saving some flash memory.
    // otherwise, leave it in.
    serial_println("MFRC self-test err: not compiled in. skipping");
    return false;
    #else

	// This follows directly the steps outlined in 16.1.1
	// 1. Perform a soft reset.
	PCD_Reset();

	// 2. Clear the internal buffer by writing 25 bytes of 00h
    uint8_t ZEROES[25] = {0x00};
	PCD_SetRegisterBitMask(FIFOLevelReg, 0x80);	// flush the FIFO buffer
	PCD_WriteRegister(FIFODataReg, 25, ZEROES);	// write 25 bytes of 00h to FIFO
	PCD_WriteRegister(CommandReg, PCD_Mem);		// transfer to internal buffer

	// 3. Enable self-test
	PCD_WriteRegister(AutoTestReg, 0x09);

	// 4. Write 00h to FIFO buffer
	PCD_WriteRegister(FIFODataReg, 0x00);

	// 5. Start self-test by issuing the CalcCRC command
	PCD_WriteRegister(CommandReg, PCD_CalcCRC);

	// 6. Wait for self-test to complete
    unsigned int i;
    uint8_t n;
	for (i = 0; i < 0xFF; i++) {
		n = PCD_ReadRegister(DivIrqReg);	// DivIrqReg[7..0] bits are: Set2 reserved reserved MfinActIRq reserved CRCIRq reserved reserved
		if (n & 0x04) {						// CRCIRq bit set - calculation done
			break;
		}
	}
	PCD_WriteRegister(CommandReg, PCD_Idle);		// Stop calculating CRC for new content in the FIFO.

	// 7. Read out resulting 64 bytes from the FIFO buffer.
    uint8_t result[64];
	PCD_ReadRegister(FIFODataReg, 64, result, 0);

	// Auto self-test done
	// Reset AutoTestReg register to be 0 again. Required for normal operation.
	PCD_WriteRegister(AutoTestReg, 0x00);

	// Determine firmware version (see section 9.3.4.8 in spec)
    uint8_t version = PCD_ReadRegister(VersionReg);

    // Pick the appropriate reference values
    const uint8_t *reference;
	switch (version) {
		case 0x88:	// Fudan Semiconductor FM17522 clone
			reference = FM17522_firmware_reference;
			break;
		case 0x90:	// Version 0.0
			reference = MFRC522_firmware_referenceV0_0;
			break;
		case 0x91:	// Version 1.0
			reference = MFRC522_firmware_referenceV1_0;
			break;
		case 0x92:	// Version 2.0
			reference = MFRC522_firmware_referenceV2_0;
			break;
		default:	// Unknown version
			return false;
	}

	// Verify that the results match up to our expectations
	for (i = 0; i < 64; i++) {
        if (result[i] != reference[i]) {
			return false;
		}
	}

	// Test passed; all is good.
	return true;
    #endif // MFRC_INCLUDE_SELFTEST
} // End PCD_PerformSelfTest()

/////////////////////////////////////////////////////////////////////////////////////
// Functions for communicating with PICCs
/////////////////////////////////////////////////////////////////////////////////////

/**
 * Executes the Transceive command.
 * CRC validation can only be done if backData and backLen are specified.
 *
 * @return STATUS_OK on success, STATUS_??? otherwise.
 */
uint8_t PCD_TransceiveData(	uint8_t *sendData,		///< Pointer to the data to transfer to the FIFO.
                                    uint8_t sendLen,		///< Number of bytes to transfer to the FIFO.
                                    uint8_t *backData,		///< NULL or pointer to buffer if data should be read back after executing the command.
                                    uint8_t *backLen,		///< In: Max number of bytes to write to *backData. Out: The number of bytes returned.
                                    uint8_t *validBits,	///< In/Out: The number of valid bits in the last byte. 0 for 8 valid bits. Default NULL.
                                    uint8_t rxAlign,		///< In: Defines the bit position in backData[0] for the first bit received. Default 0.
									bool checkCRC		///< In: True => The last two bytes of the response is assumed to be a CRC_A that must be validated.
								 ) {
    uint8_t waitIRq = 0x30;		// RxIRq and IdleIRq
	return PCD_CommunicateWithPICC(PCD_Transceive, waitIRq, sendData, sendLen, backData, backLen, validBits, rxAlign, checkCRC);
} // End PCD_TransceiveData()

/**
 * Transfers data to the MFRC522 FIFO, executes a command, waits for completion and transfers data back from the FIFO.
 * CRC validation can only be done if backData and backLen are specified.
 *
 * @return STATUS_OK on success, STATUS_??? otherwise.
 */
uint8_t PCD_CommunicateWithPICC(	uint8_t command,		///< The command to execute. One of the PCD_Command enums.
                                        uint8_t waitIRq,		///< The bits in the ComIrqReg register that signals successful completion of the command.
                                        uint8_t *sendData,		///< Pointer to the data to transfer to the FIFO.
                                        uint8_t sendLen,		///< Number of bytes to transfer to the FIFO.
                                        uint8_t *backData,		///< NULL or pointer to buffer if data should be read back after executing the command.
                                        uint8_t *backLen,		///< In: Max number of bytes to write to *backData. Out: The number of bytes returned.
                                        uint8_t *validBits,	///< In/Out: The number of valid bits in the last byte. 0 for 8 valid bits.
                                        uint8_t rxAlign,		///< In: Defines the bit position in backData[0] for the first bit received. Default 0.
										bool checkCRC		///< In: True => The last two bytes of the response is assumed to be a CRC_A that must be validated.
									 ) {
    uint8_t n=0, _validBits=0;
	unsigned int i;

	// Prepare values for BitFramingReg
    uint8_t txLastBits = validBits ? *validBits : 0;
    uint8_t bitFraming = (rxAlign << 4) + txLastBits;		// RxAlign = BitFramingReg[6..4]. TxLastBits = BitFramingReg[2..0]

	PCD_WriteRegister(CommandReg, PCD_Idle);			// Stop any active command.
	PCD_WriteRegister(ComIrqReg, 0x7F);					// Clear all seven interrupt request bits
	PCD_SetRegisterBitMask(FIFOLevelReg, 0x80);			// FlushBuffer = 1, FIFO initialization
	PCD_WriteRegisterData(FIFODataReg, sendLen, sendData);	// Write sendData to the FIFO
	PCD_WriteRegister(BitFramingReg, bitFraming);		// Bit adjustments
	PCD_WriteRegister(CommandReg, command);				// Execute the command
	if (command == PCD_Transceive) {
		PCD_SetRegisterBitMask(BitFramingReg, 0x80);	// StartSend=1, transmission of data starts
	}

	// Wait for the command to complete.
	// In PCD_Init() we set the TAuto flag in TModeReg. This means the timer automatically starts when the PCD stops transmitting.
	// Each iteration of the do-while-loop takes 17.86�s.
	i = 2000;
	while (1) {
		n = PCD_ReadRegister(ComIrqReg);	// ComIrqReg[7..0] bits are: Set1 TxIRq RxIRq IdleIRq HiAlertIRq LoAlertIRq ErrIRq TimerIRq
		if (n & waitIRq) {					// One of the interrupts that signal success has been set.
			break;
		}
		if (n & 0x01) {						// Timer interrupt - nothing received in 25ms
			return STATUS_TIMEOUT;
		}
		if (--i == 0) {						// The emergency break. If all other condions fail we will eventually terminate on this one after 35.7ms. Communication with the MFRC522 might be down.
			return STATUS_TIMEOUT;
		}
	}

	// Stop now if any errors except collisions were detected.
    uint8_t errorRegValue = PCD_ReadRegister(ErrorReg); // ErrorReg[7..0] bits are: WrErr TempErr reserved BufferOvfl CollErr CRCErr ParityErr ProtocolErr
	if (errorRegValue & 0x13) {	 // BufferOvfl ParityErr ProtocolErr
		return STATUS_ERROR;
	}

	// If the caller wants data back, get it from the MFRC522.
	if (backData && backLen) {
		n = PCD_ReadRegister(FIFOLevelReg);			// Number of bytes in the FIFO
		if (n > *backLen) {
			return STATUS_NO_ROOM;
		}
		*backLen = n;											// Number of bytes returned
		PCD_ReadRegisterData(FIFODataReg, n, backData, rxAlign);	// Get received data from FIFO
		_validBits = PCD_ReadRegister(ControlReg) & 0x07;		// RxLastBits[2:0] indicates the number of valid bits in the last received byte. If this value is 000b, the whole byte is valid.
		if (validBits) {
			*validBits = _validBits;
		}
	}

	// Tell about collisions
	if (errorRegValue & 0x08) {		// CollErr
		return STATUS_COLLISION;
	}

	// Perform CRC_A validation if requested.
	if (backData && backLen && checkCRC) {
		// In this case a MIFARE Classic NAK is not OK.
		if (*backLen == 1 && _validBits == 4) {
			return STATUS_MIFARE_NACK;
		}
		// We need at least the CRC_A value and all 8 bits of the last byte must be received.
		if (*backLen < 2 || _validBits != 0) {
			return STATUS_CRC_WRONG;
		}
		// Verify CRC_A - do our own calculation and store the control in controlBuffer.
        uint8_t controlBuffer[2];
		n = PCD_CalculateCRC(&backData[0], *backLen - 2, &controlBuffer[0]);
		if (n != STATUS_OK) {
			return n;
		}
		if ((backData[*backLen - 2] != controlBuffer[0]) || (backData[*backLen - 1] != controlBuffer[1])) {
			return STATUS_CRC_WRONG;
		}
	}

	return STATUS_OK;
} // End PCD_CommunicateWithPICC()

/**
 * Transmits a REQuest command, Type A. Invites PICCs in state IDLE to go to READY and prepare for anticollision or selection. 7 bit frame.
 * Beware: When two PICCs are in the field at the same time I often get STATUS_TIMEOUT - probably due do bad antenna design.
 *
 * @return STATUS_OK on success, STATUS_??? otherwise.
 */
uint8_t PICC_RequestA(uint8_t *bufferATQA,	///< The buffer to store the ATQA (Answer to request) in
                            uint8_t *bufferSize	///< Buffer size, at least two bytes. Also number of bytes returned if STATUS_OK.
							) {
	return PICC_REQA_or_WUPA(PICC_CMD_REQA, bufferATQA, bufferSize);
} // End PICC_RequestA()

/**
 * Transmits a Wake-UP command, Type A. Invites PICCs in state IDLE and HALT to go to READY(*) and prepare for anticollision or selection. 7 bit frame.
 * Beware: When two PICCs are in the field at the same time I often get STATUS_TIMEOUT - probably due do bad antenna design.
 *
 * @return STATUS_OK on success, STATUS_??? otherwise.
 */
uint8_t PICC_WakeupA(	uint8_t *bufferATQA,	///< The buffer to store the ATQA (Answer to request) in
                            uint8_t *bufferSize	///< Buffer size, at least two bytes. Also number of bytes returned if STATUS_OK.
							) {
	return PICC_REQA_or_WUPA(PICC_CMD_WUPA, bufferATQA, bufferSize);
} // End PICC_WakeupA()

/**
 * Transmits REQA or WUPA commands.
 * Beware: When two PICCs are in the field at the same time I often get STATUS_TIMEOUT - probably due do bad antenna design.
 *
 * @return STATUS_OK on success, STATUS_??? otherwise.
 */
uint8_t PICC_REQA_or_WUPA(	uint8_t command, 		///< The command to send - PICC_CMD_REQA or PICC_CMD_WUPA
                                    uint8_t *bufferATQA,	///< The buffer to store the ATQA (Answer to request) in
                                    uint8_t *bufferSize	///< Buffer size, at least two bytes. Also number of bytes returned if STATUS_OK.
							   ) {
    uint8_t validBits;
    uint8_t status;

	if (bufferATQA == NULL || *bufferSize < 2) {	// The ATQA response is 2 bytes long.
		return STATUS_NO_ROOM;
	}
	PCD_ClearRegisterBitMask(CollReg, 0x80);		// ValuesAfterColl=1 => Bits received after collision are cleared.
	validBits = 7;									// For REQA and WUPA we need the short frame format - transmit only 7 bits of the last (and only) byte. TxLastBits = BitFramingReg[2..0]
    const uint8_t rxAlign = 0;
    const bool checkCRC = false;
    status = PCD_TransceiveData(&command, 1, bufferATQA, bufferSize, &validBits, rxAlign, checkCRC);
	if (status != STATUS_OK) {
		return status;
	}
	if (*bufferSize != 2 || validBits != 0) {		// ATQA must be exactly 16 bits.
		return STATUS_ERROR;
	}
	return STATUS_OK;
} // End PICC_REQA_or_WUPA()

/**
 * Transmits SELECT/ANTICOLLISION commands to select a single PICC.
 * Before calling this function the PICCs must be placed in the READY(*) state by calling PICC_RequestA() or PICC_WakeupA().
 * On success:
 * 		- The chosen PICC is in state ACTIVE(*) and all other PICCs have returned to state IDLE/HALT. (Figure 7 of the ISO/IEC 14443-3 draft.)
 * 		- The UID size and value of the chosen PICC is returned in *uid along with the SAK.
 *
 * A PICC UID consists of 4, 7 or 10 bytes.
 * Only 4 bytes can be specified in a SELECT command, so for the longer UIDs two or three iterations are used:
 * 		UID size	Number of UID bytes		Cascade levels		Example of PICC
 * 		========	===================		==============		===============
 * 		single				 4						1				MIFARE Classic
 * 		double				 7						2				MIFARE Ultralight
 * 		triple				10						3				Not currently in use?
 *
 * @return STATUS_OK on success, STATUS_??? otherwise.
 */
uint8_t PICC_Select(	Uid *uid,			///< Pointer to Uid struct. Normally output, but can also be used to supply a known UID.
                        uint8_t validBits		///< The number of known UID bits supplied in *uid. Normally 0. If set you must also supply uid->size.
						 ) {
	bool uidComplete;
	bool selectDone;
	bool useCascadeTag;
    uint8_t cascadeLevel = 1;
    uint8_t result;
    uint8_t count;
    uint8_t index;
    uint8_t uidIndex;					// The first index in uid->uidByte[] that is used in the current Cascade Level.
	int8_t currentLevelKnownBits;		// The number of known UID bits in the current Cascade Level.
    uint8_t buffer[9];					// The SELECT/ANTICOLLISION commands uses a 7 byte standard frame + 2 bytes CRC_A
    uint8_t bufferUsed;				// The number of bytes used in the buffer, ie the number of bytes to transfer to the FIFO.
    uint8_t rxAlign;					// Used in BitFramingReg. Defines the bit position for the first bit received.
    uint8_t txLastBits;				// Used in BitFramingReg. The number of valid bits in the last transmitted byte.
    uint8_t *responseBuffer;
    uint8_t responseLength;

	// Description of buffer structure:
	//		Byte 0: SEL 				Indicates the Cascade Level: PICC_CMD_SEL_CL1, PICC_CMD_SEL_CL2 or PICC_CMD_SEL_CL3
	//		Byte 1: NVB					Number of Valid Bits (in complete command, not just the UID): High nibble: complete bytes, Low nibble: Extra bits.
	//		Byte 2: UID-data or CT		See explanation below. CT means Cascade Tag.
	//		Byte 3: UID-data
	//		Byte 4: UID-data
	//		Byte 5: UID-data
	//		Byte 6: BCC					Block Check Character - XOR of bytes 2-5
	//		Byte 7: CRC_A
	//		Byte 8: CRC_A
	// The BCC and CRC_A is only transmitted if we know all the UID bits of the current Cascade Level.
	//
	// Description of bytes 2-5: (Section 6.5.4 of the ISO/IEC 14443-3 draft: UID contents and cascade levels)
	//		UID size	Cascade level	Byte2	Byte3	Byte4	Byte5
	//		========	=============	=====	=====	=====	=====
	//		 4 bytes		1			uid0	uid1	uid2	uid3
	//		 7 bytes		1			CT		uid0	uid1	uid2
	//						2			uid3	uid4	uid5	uid6
	//		10 bytes		1			CT		uid0	uid1	uid2
	//						2			CT		uid3	uid4	uid5
	//						3			uid6	uid7	uid8	uid9

	// Sanity checks
	if (validBits > 80) {
		return STATUS_INVALID;
	}

	// Prepare MFRC522
	PCD_ClearRegisterBitMask(CollReg, 0x80);		// ValuesAfterColl=1 => Bits received after collision are cleared.

	// Repeat Cascade Level loop until we have a complete UID.
	uidComplete = false;
	while (!uidComplete) {
		// Set the Cascade Level in the SEL byte, find out if we need to use the Cascade Tag in byte 2.
		switch (cascadeLevel) {
			case 1:
				buffer[0] = PICC_CMD_SEL_CL1;
				uidIndex = 0;
				useCascadeTag = validBits && uid->size > 4;	// When we know that the UID has more than 4 bytes
				break;

			case 2:
				buffer[0] = PICC_CMD_SEL_CL2;
				uidIndex = 3;
				useCascadeTag = validBits && uid->size > 7;	// When we know that the UID has more than 7 bytes
				break;

			case 3:
				buffer[0] = PICC_CMD_SEL_CL3;
				uidIndex = 6;
				useCascadeTag = false;						// Never used in CL3.
				break;

			default:
				return STATUS_INTERNAL_ERROR;
		}

		// How many UID bits are known in this Cascade Level?
		currentLevelKnownBits = validBits - (8 * uidIndex);
		if (currentLevelKnownBits < 0) {
			currentLevelKnownBits = 0;
		}
		// Copy the known bits from uid->uidByte[] to buffer[]
		index = 2; // destination index in buffer[]
		if (useCascadeTag) {
			buffer[index++] = PICC_CMD_CT;
		}
        uint8_t bytesToCopy = currentLevelKnownBits / 8 + (currentLevelKnownBits % 8 ? 1 : 0); // The number of bytes needed to represent the known bits for this level.
		if (bytesToCopy) {
            uint8_t maxBytes = useCascadeTag ? 3 : 4; // Max 4 bytes in each Cascade Level. Only 3 left if we use the Cascade Tag
			if (bytesToCopy > maxBytes) {
				bytesToCopy = maxBytes;
			}
			for (count = 0; count < bytesToCopy; count++) {
				buffer[index++] = uid->uidByte[uidIndex + count];
			}
		}
		// Now that the data has been copied we need to include the 8 bits in CT in currentLevelKnownBits
		if (useCascadeTag) {
			currentLevelKnownBits += 8;
		}

		// Repeat anti collision loop until we can transmit all UID bits + BCC and receive a SAK - max 32 iterations.
		selectDone = false;
		while (!selectDone) {
			// Find out how many bits and bytes to send and receive.
			if (currentLevelKnownBits >= 32) { // All UID bits in this Cascade Level are known. This is a SELECT.
                if (g_mfrc._logDebugInfo) {
                    serial_print("SELECT: currentLevelKnownBits=");serial_println_f(currentLevelKnownBits, DEC);
                }
				buffer[1] = 0x70; // NVB - Number of Valid Bits: Seven whole bytes
				// Calculate BCC - Block Check Character
				buffer[6] = buffer[2] ^ buffer[3] ^ buffer[4] ^ buffer[5];
				// Calculate CRC_A
				result = PCD_CalculateCRC(buffer, 7, &buffer[7]);
				if (result != STATUS_OK) {
					return result;
				}
				txLastBits		= 0; // 0 => All 8 bits are valid.
				bufferUsed		= 9;
				// Store response in the last 3 bytes of buffer (BCC and CRC_A - not needed after tx)
				responseBuffer	= &buffer[6];
				responseLength	= 3;
			}
			else { // This is an ANTICOLLISION.
                if (g_mfrc._logDebugInfo) {
                    serial_print("ANTICOLLISION: currentLevelKnownBits="); serial_println_f(currentLevelKnownBits, DEC);
                }
				txLastBits		= currentLevelKnownBits % 8;
				count			= currentLevelKnownBits / 8;	// Number of whole bytes in the UID part.
				index			= 2 + count;					// Number of whole bytes: SEL + NVB + UIDs
				buffer[1]		= (index << 4) + txLastBits;	// NVB - Number of Valid Bits
				bufferUsed		= index + (txLastBits ? 1 : 0);
				// Store response in the unused part of buffer
				responseBuffer	= &buffer[index];
				responseLength	= sizeof(buffer) - index;
			}

			// Set bit adjustments
			rxAlign = txLastBits;											// Having a seperate variable is overkill. But it makes the next line easier to read.
			PCD_WriteRegister(BitFramingReg, (rxAlign << 4) + txLastBits);	// RxAlign = BitFramingReg[6..4]. TxLastBits = BitFramingReg[2..0]

			// Transmit the buffer and receive the response.
            const bool checkCRC = false;
			result = PCD_TransceiveData(buffer, bufferUsed, responseBuffer, &responseLength, &txLastBits, rxAlign, checkCRC);
			if (result == STATUS_COLLISION) { // More than one PICC in the field => collision.
				result = PCD_ReadRegister(CollReg); // CollReg[7..0] bits are: ValuesAfterColl reserved CollPosNotValid CollPos[4:0]
				if (result & 0x20) { // CollPosNotValid
					return STATUS_COLLISION; // Without a valid collision position we cannot continue
				}
                uint8_t collisionPos = result & 0x1F; // Values 0-31, 0 means bit 32.
				if (collisionPos == 0) {
					collisionPos = 32;
				}
				if (collisionPos <= currentLevelKnownBits) { // No progress - should not happen
					return STATUS_INTERNAL_ERROR;
				}
				// Choose the PICC with the bit set.
				currentLevelKnownBits = collisionPos;
				count			= (currentLevelKnownBits - 1) % 8; // The bit to modify
				index			= 1 + (currentLevelKnownBits / 8) + (count ? 1 : 0); // First byte is index 0.
				buffer[index]	|= (1 << count);
			}
			else if (result != STATUS_OK) {
				return result;
			}
			else { // STATUS_OK
				if (currentLevelKnownBits >= 32) { // This was a SELECT.
					selectDone = true; // No more anticollision
					// We continue below outside the while.
				}
				else { // This was an ANTICOLLISION.
					// We now have all 32 bits of the UID in this Cascade Level
					currentLevelKnownBits = 32;
					// Run loop again to do the SELECT.
				}
			}
		} // End of while (!selectDone)

		// We do not check the CBB - it was constructed by us above.

		// Copy the found UID bytes from buffer[] to uid->uidByte[]
		index			= (buffer[2] == PICC_CMD_CT) ? 3 : 2; // source index in buffer[]
		bytesToCopy		= (buffer[2] == PICC_CMD_CT) ? 3 : 4;
		for (count = 0; count < bytesToCopy; count++) {
			uid->uidByte[uidIndex + count] = buffer[index++];
		}

		// Check response SAK (Select Acknowledge)
		if (responseLength != 3 || txLastBits != 0) { // SAK must be exactly 24 bits (1 byte + CRC_A).
			return STATUS_ERROR;
		}
		// Verify CRC_A - do our own calculation and store the control in buffer[2..3] - those bytes are not needed anymore.
		result = PCD_CalculateCRC(responseBuffer, 1, &buffer[2]);
		if (result != STATUS_OK) {
			return result;
		}
		if ((buffer[2] != responseBuffer[1]) || (buffer[3] != responseBuffer[2])) {
			return STATUS_CRC_WRONG;
		}

        // TODO: GCC complaining that error: 'responseBuffer' may be used uninitialized on the line below.
        //       we should investigate why that's happening and fix it if it's legit.
        #pragma GCC diagnostic push
        #pragma GCC diagnostic warning "-Wmaybe-uninitialized"
		if (responseBuffer[0] & 0x04) { // Cascade bit set - UID not complete yes
			cascadeLevel++;
		}
		else {
			uidComplete = true;
			uid->sak = responseBuffer[0];
		}
        #pragma GCC diagnostic pop

	} // End of while (!uidComplete)

	// Set correct uid->size
	uid->size = 3 * cascadeLevel + 1;

	return STATUS_OK;
} // End PICC_Select()

/**
 * Instructs a PICC in state ACTIVE(*) to go to state HALT.
 *
 * @return STATUS_OK on success, STATUS_??? otherwise.
 */
uint8_t PICC_HaltA() {
    uint8_t result;
    uint8_t buffer[4];

	// Build command buffer
	buffer[0] = PICC_CMD_HLTA;
	buffer[1] = 0;
	// Calculate CRC_A
	result = PCD_CalculateCRC(buffer, 2, &buffer[2]);
	if (result != STATUS_OK) {
		return result;
	}

	// Send the command.
	// The standard says:
	//		If the PICC responds with any modulation during a period of 1 ms after the end of the frame containing the
	//		HLTA command, this response shall be interpreted as 'not acknowledge'.
	// We interpret that this way: Only STATUS_TIMEOUT is an success.
	result = PCD_TransceiveData(buffer, sizeof(buffer), NULL, 0, NULL, 0, false);
	if (result == STATUS_TIMEOUT) {
		return STATUS_OK;
	}
	if (result == STATUS_OK) { // That is ironically NOT ok in this case ;-)
		return STATUS_ERROR;
	}
	return result;
} // End PICC_HaltA()


/////////////////////////////////////////////////////////////////////////////////////
// Functions for communicating with MIFARE PICCs
/////////////////////////////////////////////////////////////////////////////////////

/**
 * Executes the MFRC522 MFAuthent command.
 * This command manages MIFARE authentication to enable a secure communication to any MIFARE Mini, MIFARE 1K and MIFARE 4K card.
 * The authentication is described in the MFRC522 datasheet section 10.3.1.9 and http://www.nxp.com/documents/data_sheet/MF1S503x.pdf section 10.1.
 * For use with MIFARE Classic PICCs.
 * The PICC must be selected - ie in state ACTIVE(*) - before calling this function.
 * Remember to call PCD_StopCrypto1() after communicating with the authenticated PICC - otherwise no new communications can start.
 *
 * All keys are set to FFFFFFFFFFFFh at chip delivery.
 *
 * @return STATUS_OK on success, STATUS_??? otherwise. Probably STATUS_TIMEOUT if you supply the wrong key.
 */
uint8_t PCD_Authenticate(uint8_t command,		///< PICC_CMD_MF_AUTH_KEY_A or PICC_CMD_MF_AUTH_KEY_B
                                uint8_t blockAddr, 	///< The block number. See numbering in the comments in the .h file.
								MIFARE_Key *key,	///< Pointer to the Crypteo1 key to use (6 bytes)
								Uid *uid			///< Pointer to Uid struct. The first 4 bytes of the UID is used.
								) {
    uint8_t waitIRq = 0x10;		// IdleIRq

	// Build command buffer
    uint8_t sendData[12];
	sendData[0] = command;
	sendData[1] = blockAddr;
    for (uint8_t i = 0; i < MF_KEY_SIZE; i++) {	// 6 key bytes
		sendData[2+i] = key->keyByte[i];
	}
	for (uint8_t i = 0; i < 4; i++) {				// The last 4 bytes of the UID
		sendData[8+i] = uid->uidByte[i+uid->size-4];
	}

	// Start the authentication.
    return PCD_CommunicateWithPICC(PCD_MFAuthent, waitIRq, &sendData[0], sizeof(sendData), NULL, NULL, NULL, 0, false);
} // End PCD_Authenticate()

/**
 * Used to exit the PCD from its authenticated state.
 * Remember to call this function after communicating with an authenticated PICC - otherwise no new communications can start.
 */
void PCD_StopCrypto1() {
	// Clear MFCrypto1On bit
	PCD_ClearRegisterBitMask(Status2Reg, 0x08); // Status2Reg[7..0] bits are: TempSensClear I2CForceHS reserved reserved MFCrypto1On ModemState[2:0]
} // End PCD_StopCrypto1()

/**
 * Reads 16 bytes (+ 2 bytes CRC_A) from the active PICC.
 *
 * For MIFARE Classic the sector containing the block must be authenticated before calling this function.
 *
 * For MIFARE Ultralight only addresses 00h to 0Fh are decoded.
 * The MF0ICU1 returns a NAK for higher addresses.
 * The MF0ICU1 responds to the READ command by sending 16 bytes starting from the page address defined by the command argument.
 * For example; if blockAddr is 03h then pages 03h, 04h, 05h, 06h are returned.
 * A roll-back is implemented: If blockAddr is 0Eh, then the contents of pages 0Eh, 0Fh, 00h and 01h are returned.
 *
 * The buffer must be at least 18 bytes because a CRC_A is also returned.
 * Checks the CRC_A before returning STATUS_OK.
 *
 * @return STATUS_OK on success, STATUS_??? otherwise.
 */
uint8_t MIFARE_Read(	uint8_t blockAddr, 	///< MIFARE Classic: The block (0-0xff) number. MIFARE Ultralight: The first page to return data from.
                            uint8_t *buffer,		///< The buffer to store the data in
                            uint8_t *bufferSize	///< Buffer size, at least 18 bytes. Also number of bytes returned if STATUS_OK.
						) {
    uint8_t result;

	// Sanity check
	if (buffer == NULL || *bufferSize < 18) {
		return STATUS_NO_ROOM;
	}

	// Build command buffer
	buffer[0] = PICC_CMD_MF_READ;
	buffer[1] = blockAddr;
	// Calculate CRC_A
	result = PCD_CalculateCRC(buffer, 2, &buffer[2]);
	if (result != STATUS_OK) {
		return result;
	}

	// Transmit the buffer and receive the response, validate CRC_A.
	return PCD_TransceiveData(buffer, 4, buffer, bufferSize, NULL, 0, true);
} // End MIFARE_Read()

/**
 * Writes 16 bytes to the active PICC.
 *
 * For MIFARE Classic the sector containing the block must be authenticated before calling this function.
 *
 * For MIFARE Ultralight the operation is called "COMPATIBILITY WRITE".
 * Even though 16 bytes are transferred to the Ultralight PICC, only the least significant 4 bytes (bytes 0 to 3)
 * are written to the specified address. It is recommended to set the remaining bytes 04h to 0Fh to all logic 0.
 * *
 * @return STATUS_OK on success, STATUS_??? otherwise.
 */
uint8_t MIFARE_Write(	uint8_t blockAddr, ///< MIFARE Classic: The block (0-0xff) number. MIFARE Ultralight: The page (2-15) to write to.
                            uint8_t *buffer,	///< The 16 bytes to write to the PICC
                            uint8_t bufferSize	///< Buffer size, must be at least 16 bytes. Exactly 16 bytes are written.
						) {
    uint8_t result;

	// Sanity check
	if (buffer == NULL || bufferSize < 16) {
		return STATUS_INVALID;
	}

	// Mifare Classic protocol requires two communications to perform a write.
	// Step 1: Tell the PICC we want to write to block blockAddr.
    uint8_t cmdBuffer[2];
	cmdBuffer[0] = PICC_CMD_MF_WRITE;
	cmdBuffer[1] = blockAddr;
	result = PCD_MIFARE_Transceive(cmdBuffer, 2, false); // Adds CRC_A and checks that the response is MF_ACK.
	if (result != STATUS_OK) {
		return result;
	}

	// Step 2: Transfer the data
	result = PCD_MIFARE_Transceive(buffer, bufferSize, false); // Adds CRC_A and checks that the response is MF_ACK.
	if (result != STATUS_OK) {
		return result;
	}

	return STATUS_OK;
} // End MIFARE_Write()

/**
 * Writes a 4 byte page to the active MIFARE Ultralight PICC.
 *
 * @return STATUS_OK on success, STATUS_??? otherwise.
 */
uint8_t MIFARE_Ultralight_Write(	uint8_t page, 		///< The page (2-15) to write to.
                                    uint8_t *buffer,	///< The 4 bytes to write to the PICC
                                    uint8_t bufferSize	///< Buffer size, must be at least 4 bytes. Exactly 4 bytes are written.
									) {
    uint8_t result;

	// Sanity check
	if (buffer == NULL || bufferSize < 4) {
		return STATUS_INVALID;
	}

	// Build commmand buffer
    uint8_t cmdBuffer[6];
	cmdBuffer[0] = PICC_CMD_UL_WRITE;
	cmdBuffer[1] = page;
	memcpy(&cmdBuffer[2], buffer, 4);

	// Perform the write
	result = PCD_MIFARE_Transceive(cmdBuffer, 6, false); // Adds CRC_A and checks that the response is MF_ACK.
	if (result != STATUS_OK) {
		return result;
	}
	return STATUS_OK;
} // End MIFARE_Ultralight_Write()

/**
 * MIFARE Decrement subtracts the delta from the value of the addressed block, and stores the result in a volatile memory.
 * For MIFARE Classic only. The sector containing the block must be authenticated before calling this function.
 * Only for blocks in "value block" mode, ie with access bits [C1 C2 C3] = [110] or [001].
 * Use MIFARE_Transfer() to store the result in a block.
 *
 * @return STATUS_OK on success, STATUS_??? otherwise.
 */
uint8_t MIFARE_Decrement(	uint8_t blockAddr, ///< The block (0-0xff) number.
								long delta		///< This number is subtracted from the value of block blockAddr.
							) {
	return MIFARE_TwoStepHelper(PICC_CMD_MF_DECREMENT, blockAddr, delta);
} // End MIFARE_Decrement()

/**
 * MIFARE Increment adds the delta to the value of the addressed block, and stores the result in a volatile memory.
 * For MIFARE Classic only. The sector containing the block must be authenticated before calling this function.
 * Only for blocks in "value block" mode, ie with access bits [C1 C2 C3] = [110] or [001].
 * Use MIFARE_Transfer() to store the result in a block.
 *
 * @return STATUS_OK on success, STATUS_??? otherwise.
 */
uint8_t MIFARE_Increment(	uint8_t blockAddr, ///< The block (0-0xff) number.
								long delta		///< This number is added to the value of block blockAddr.
							) {
	return MIFARE_TwoStepHelper(PICC_CMD_MF_INCREMENT, blockAddr, delta);
} // End MIFARE_Increment()

/**
 * MIFARE Restore copies the value of the addressed block into a volatile memory.
 * For MIFARE Classic only. The sector containing the block must be authenticated before calling this function.
 * Only for blocks in "value block" mode, ie with access bits [C1 C2 C3] = [110] or [001].
 * Use MIFARE_Transfer() to store the result in a block.
 *
 * @return STATUS_OK on success, STATUS_??? otherwise.
 */
uint8_t MIFARE_Restore(	uint8_t blockAddr ///< The block (0-0xff) number.
							) {
	// The datasheet describes Restore as a two step operation, but does not explain what data to transfer in step 2.
	// Doing only a single step does not work, so I chose to transfer 0L in step two.
	return MIFARE_TwoStepHelper(PICC_CMD_MF_RESTORE, blockAddr, 0L);
} // End MIFARE_Restore()

/**
 * Helper function for the two-step MIFARE Classic protocol operations Decrement, Increment and Restore.
 *
 * @return STATUS_OK on success, STATUS_??? otherwise.
 */
uint8_t MIFARE_TwoStepHelper(	uint8_t command,	///< The command to use
                                    uint8_t blockAddr,	///< The block (0-0xff) number.
									long data		///< The data to transfer in step 2
									) {
    uint8_t result;
    uint8_t cmdBuffer[2]; // We only need room for 2 bytes.

	// Step 1: Tell the PICC the command and block address
	cmdBuffer[0] = command;
	cmdBuffer[1] = blockAddr;
	result = PCD_MIFARE_Transceive(	cmdBuffer, 2, false); // Adds CRC_A and checks that the response is MF_ACK.
	if (result != STATUS_OK) {
		return result;
	}

	// Step 2: Transfer the data
    result = PCD_MIFARE_Transceive(	(uint8_t *)&data, 4, true); // Adds CRC_A and accept timeout as success.
	if (result != STATUS_OK) {
		return result;
	}

	return STATUS_OK;
} // End MIFARE_TwoStepHelper()

/**
 * MIFARE Transfer writes the value stored in the volatile memory into one MIFARE Classic block.
 * For MIFARE Classic only. The sector containing the block must be authenticated before calling this function.
 * Only for blocks in "value block" mode, ie with access bits [C1 C2 C3] = [110] or [001].
 *
 * @return STATUS_OK on success, STATUS_??? otherwise.
 */
uint8_t MIFARE_Transfer(	uint8_t blockAddr ///< The block (0-0xff) number.
							) {
    uint8_t result;
    uint8_t cmdBuffer[2]; // We only need room for 2 bytes.

	// Tell the PICC we want to transfer the result into block blockAddr.
	cmdBuffer[0] = PICC_CMD_MF_TRANSFER;
	cmdBuffer[1] = blockAddr;
	result = PCD_MIFARE_Transceive(	cmdBuffer, 2, false); // Adds CRC_A and checks that the response is MF_ACK.
	if (result != STATUS_OK) {
		return result;
	}
	return STATUS_OK;
} // End MIFARE_Transfer()

/**
 * Helper routine to read the current value from a Value Block.
 *
 * Only for MIFARE Classic and only for blocks in "value block" mode, that
 * is: with access bits [C1 C2 C3] = [110] or [001]. The sector containing
 * the block must be authenticated before calling this function.
 *
 * @param[in]   blockAddr   The block (0x00-0xff) number.
 * @param[out]  value       Current value of the Value Block.
 * @return STATUS_OK on success, STATUS_??? otherwise.
  */
uint8_t MIFARE_GetValue(uint8_t blockAddr, long *value) {
    uint8_t status;
    uint8_t buffer[18];
    uint8_t size = sizeof(buffer);

	// Read the block
	status = MIFARE_Read(blockAddr, buffer, &size);
	if (status == STATUS_OK) {
		// Extract the value
		*value =
                ((long)((buffer[3])<<24)) |
                ((long)((buffer[2])<<16)) |
                ((long)((buffer[1])<<8)) |
                ((long)(buffer[0]));
	}
	return status;
} // End MIFARE_GetValue()

/**
 * Helper routine to write a specific value into a Value Block.
 *
 * Only for MIFARE Classic and only for blocks in "value block" mode, that
 * is: with access bits [C1 C2 C3] = [110] or [001]. The sector containing
 * the block must be authenticated before calling this function.
 *
 * @param[in]   blockAddr   The block (0x00-0xff) number.
 * @param[in]   value       New value of the Value Block.
 * @return STATUS_OK on success, STATUS_??? otherwise.
 */
uint8_t MIFARE_SetValue(uint8_t blockAddr, long value) {
    uint8_t buffer[18];

	// Translate the long into 4 bytes; repeated 2x in value block
	buffer[0] = buffer[ 8] = (value & 0xFF);
	buffer[1] = buffer[ 9] = (value & 0xFF00) >> 8;
	buffer[2] = buffer[10] = (value & 0xFF0000) >> 16;
	buffer[3] = buffer[11] = (value & 0xFF000000) >> 24;
	// Inverse 4 bytes also found in value block
	buffer[4] = ~buffer[0];
	buffer[5] = ~buffer[1];
	buffer[6] = ~buffer[2];
	buffer[7] = ~buffer[3];
	// Address 2x with inverse address 2x
	buffer[12] = buffer[14] = blockAddr;
	buffer[13] = buffer[15] = ~blockAddr;

	// Write the whole data block
	return MIFARE_Write(blockAddr, buffer, 16);
} // End MIFARE_SetValue()

/////////////////////////////////////////////////////////////////////////////////////
// Support functions
/////////////////////////////////////////////////////////////////////////////////////

/**
 * Wrapper for MIFARE protocol communication.
 * Adds CRC_A, executes the Transceive command and checks that the response is MF_ACK or a timeout.
 *
 * @return STATUS_OK on success, STATUS_??? otherwise.
 */
uint8_t PCD_MIFARE_Transceive(	uint8_t *sendData,		///< Pointer to the data to transfer to the FIFO. Do NOT include the CRC_A.
                                        uint8_t sendLen,		///< Number of bytes in sendData.
										bool acceptTimeout	///< True => A timeout is also success
									) {
    uint8_t result;
    uint8_t cmdBuffer[18]; // We need room for 16 bytes data and 2 bytes CRC_A.

	// Sanity check
	if (sendData == NULL || sendLen > 16) {
		return STATUS_INVALID;
	}

	// Copy sendData[] to cmdBuffer[] and add CRC_A
	memcpy(cmdBuffer, sendData, sendLen);
	result = PCD_CalculateCRC(cmdBuffer, sendLen, &cmdBuffer[sendLen]);
	if (result != STATUS_OK) {
		return result;
	}
	sendLen += 2;

	// Transceive the data, store the reply in cmdBuffer[]
    uint8_t waitIRq = 0x30;		// RxIRq and IdleIRq
    uint8_t cmdBufferSize = sizeof(cmdBuffer);
    uint8_t validBits = 0;
    const uint8_t rxAlign = 0;
    const bool checkCRC = false;
	result = PCD_CommunicateWithPICC(PCD_Transceive, waitIRq, cmdBuffer, sendLen, cmdBuffer, &cmdBufferSize, &validBits, rxAlign, checkCRC);
	if (acceptTimeout && result == STATUS_TIMEOUT) {
		return STATUS_OK;
	}
	if (result != STATUS_OK) {
		return result;
	}
	// The PICC must reply with a 4 bit ACK
	if (cmdBufferSize != 1 || validBits != 4) {
		return STATUS_ERROR;
	}
	if (cmdBuffer[0] != MF_ACK) {
		return STATUS_MIFARE_NACK;
	}
	return STATUS_OK;
} // End PCD_MIFARE_Transceive()

/**
 * Returns a __FlashStringHelper pointer to a status code name.
 *
 * @return const __FlashStringHelper *
 */
const char *GetStatusCodeName(uint8_t code	///< One of the StatusCode enums.
										) {
	switch (code) {
        case STATUS_OK:				return "Success.";
        case STATUS_ERROR:			return "Error in communication.";
        case STATUS_COLLISION:		return "Collission detected.";
        case STATUS_TIMEOUT:		return "Timeout in communication.";
        case STATUS_NO_ROOM:		return "A buffer is not big enough.";
        case STATUS_INTERNAL_ERROR:	return "Internal error in the code. Should not happen.";
        case STATUS_INVALID:		return "Invalid argument.";
        case STATUS_CRC_WRONG:		return "The CRC_A does not match.";
        case STATUS_MIFARE_NACK:	return "A MIFARE PICC responded with NAK.";
        default:					return "Unknown error";
	}
} // End GetStatusCodeName()

/**
 * Translates the SAK (Select Acknowledge) to a PICC type.
 *
 * @return PICC_Type
 */
uint8_t PICC_GetType(uint8_t sak		///< The SAK byte returned from PICC_Select().
							) {
	if (sak & 0x04) { // UID not complete
		return PICC_TYPE_NOT_COMPLETE;
	}

	switch (sak) {
		case 0x09:	return PICC_TYPE_MIFARE_MINI;
		case 0x08:	return PICC_TYPE_MIFARE_1K;
		case 0x18:	return PICC_TYPE_MIFARE_4K;
		case 0x00:	return PICC_TYPE_MIFARE_UL;
		case 0x10:
		case 0x11:	return PICC_TYPE_MIFARE_PLUS;
		case 0x01:	return PICC_TYPE_TNP3XXX;
		default:	break;
	}

	if (sak & 0x20) {
		return PICC_TYPE_ISO_14443_4;
	}

	if (sak & 0x40) {
		return PICC_TYPE_ISO_18092;
	}

	return PICC_TYPE_UNKNOWN;
} // End PICC_GetType()

/**
 * Returns a __FlashStringHelper pointer to the PICC type name.
 *
 * @return const __FlashStringHelper *
 */
const char *PICC_GetTypeName(uint8_t piccType	///< One of the PICC_Type enums.
										) {
	switch (piccType) {
        case PICC_TYPE_ISO_14443_4:		return "PICC compliant with ISO/IEC 14443-4";
        case PICC_TYPE_ISO_18092:		return "PICC compliant with ISO/IEC 18092 (NFC)";
        case PICC_TYPE_MIFARE_MINI:		return "MIFARE Mini, 320 bytes";
        case PICC_TYPE_MIFARE_1K:		return "MIFARE 1KB";
        case PICC_TYPE_MIFARE_4K:		return "MIFARE 4KB";
        case PICC_TYPE_MIFARE_UL:		return "MIFARE Ultralight or Ultralight C";
        case PICC_TYPE_MIFARE_PLUS:		return "MIFARE Plus";
        case PICC_TYPE_TNP3XXX:			return "MIFARE TNP3XXX";
        case PICC_TYPE_NOT_COMPLETE:	return "SAK indicates UID is not complete.";
		case PICC_TYPE_UNKNOWN:
        default:						return "Unknown type";
	}
} // End PICC_GetTypeName()

/**
 * Dumps debug info about the connected PCD to serial_
 * Shows all known firmware versions
 */
void PCD_DumpVersionToSerial() {
	// Get the MFRC522 firmware version
    uint8_t v = PCD_GetVersion();
    serial_print("MFRC522 Firmware Version Detected: 0x");
	serial_print_f(v, HEX);
	// Lookup which version
	switch(v) {
		case 0x88: serial_println(" = (clone)");  break;
		case 0x90: serial_println(" = v0.0");     break;
		case 0x91: serial_println(" = v1.0");     break;
		case 0x92: serial_println(" = v2.0");     break;
		case 0x12: serial_println(" = counterfeit chip");     break;
		default:   serial_println(" = (unknown)");
	}
	// When 0x00 or 0xFF is returned, communication probably failed
	if ((v == 0x00) || (v == 0xFF))
		serial_println("WARNING: Communication failure, is the MFRC522 properly connected?");
}

uint8_t PCD_GetVersion() {
    return PCD_ReadRegister(VersionReg);
}

// End PCD_DumpVersionToSerial()

/**
 * Dumps debug info about the selected PICC to serial_
 * On success the PICC is halted after dumping the data.
 * For MIFARE Classic the factory default key of 0xFFFFFFFFFFFF is tried.
 */
void PICC_DumpToSerial(Uid *uid	///< Pointer to Uid struct returned from a successful PICC_Select().
								) {
	MIFARE_Key key;

	// UID
    serial_print("Card UID:");
    for (uint8_t i = 0; i < uid->size; i++) {
		if(uid->uidByte[i] < 0x10)
            serial_print(" 0");
		else
            serial_print(" ");
		serial_print_f(uid->uidByte[i], HEX);
	}
	serial_println("");

	// PICC type
    uint8_t piccType = PICC_GetType(uid->sak);
    serial_print("PICC type: ");
	serial_println(PICC_GetTypeName(piccType));

	// Dump contents
	switch (piccType) {
		case PICC_TYPE_MIFARE_MINI:
		case PICC_TYPE_MIFARE_1K:
		case PICC_TYPE_MIFARE_4K:
			// All keys are set to FFFFFFFFFFFFh at chip delivery from the factory.
            for (uint8_t i = 0; i < 6; i++) {
				key.keyByte[i] = 0xFF;
			}
			PICC_DumpMifareClassicToSerial(uid, piccType, &key);
			break;

		case PICC_TYPE_MIFARE_UL:
			PICC_DumpMifareUltralightToSerial();
			break;

		case PICC_TYPE_ISO_14443_4:
		case PICC_TYPE_ISO_18092:
		case PICC_TYPE_MIFARE_PLUS:
		case PICC_TYPE_TNP3XXX:
            serial_println("Dumping memory contents not implemented for that PICC type.");
			break;

		case PICC_TYPE_UNKNOWN:
		case PICC_TYPE_NOT_COMPLETE:
		default:
			break; // No memory dump here
	}

    serial_println("");
	PICC_HaltA(); // Already done if it was a MIFARE Classic PICC.
} // End PICC_DumpToSerial()

/**
 * Dumps memory contents of a MIFARE Classic PICC.
 * On success the PICC is halted after dumping the data.
 */
void PICC_DumpMifareClassicToSerial(	Uid *uid,		    ///< Pointer to Uid struct returned from a successful PICC_Select().
                                        uint8_t piccType,	///< One of the PICC_Type enums.
                                        MIFARE_Key *key	    ///< Key A used for all sectors.
											) {
    uint8_t no_of_sectors = 0;
	switch (piccType) {
		case PICC_TYPE_MIFARE_MINI:
			// Has 5 sectors * 4 blocks/sector * 16 bytes/block = 320 bytes.
			no_of_sectors = 5;
			break;

		case PICC_TYPE_MIFARE_1K:
			// Has 16 sectors * 4 blocks/sector * 16 bytes/block = 1024 bytes.
			no_of_sectors = 16;
			break;

		case PICC_TYPE_MIFARE_4K:
			// Has (32 sectors * 4 blocks/sector + 8 sectors * 16 blocks/sector) * 16 bytes/block = 4096 bytes.
			no_of_sectors = 40;
			break;

		default: // Should not happen. Ignore.
			break;
	}

	// Dump sectors, highest address first.
	if (no_of_sectors) {
        serial_println("Sector Block   0  1  2  3   4  5  6  7   8  9 10 11  12 13 14 15  AccessBits");
		for (int8_t i = no_of_sectors - 1; i >= 0; i--) {
			PICC_DumpMifareClassicSectorToSerial(uid, key, i);
		}
	}
	PICC_HaltA(); // Halt the PICC before stopping the encrypted session.
	PCD_StopCrypto1();
} // End PICC_DumpMifareClassicToSerial()

/**
 * Dumps memory contents of a sector of a MIFARE Classic PICC.
 * Uses PCD_Authenticate(), MIFARE_Read() and PCD_StopCrypto1.
 * Always uses PICC_CMD_MF_AUTH_KEY_A because only Key A can always read the sector trailer access bits.
 */
void PICC_DumpMifareClassicSectorToSerial(Uid *uid,			///< Pointer to Uid struct returned from a successful PICC_Select().
													MIFARE_Key *key,	///< Key A for the sector.
                                                    uint8_t sector			///< The sector to dump, 0..39.
													) {
    uint8_t status;
    uint8_t firstBlock;		// Address of lowest address to dump actually last block dumped)
    uint8_t no_of_blocks;		// Number of blocks in sector
	bool isSectorTrailer;	// Set to true while handling the "last" (ie highest address) in the sector.

	// The access bits are stored in a peculiar fashion.
	// There are four groups:
	//		g[3]	Access bits for the sector trailer, block 3 (for sectors 0-31) or block 15 (for sectors 32-39)
	//		g[2]	Access bits for block 2 (for sectors 0-31) or blocks 10-14 (for sectors 32-39)
	//		g[1]	Access bits for block 1 (for sectors 0-31) or blocks 5-9 (for sectors 32-39)
	//		g[0]	Access bits for block 0 (for sectors 0-31) or blocks 0-4 (for sectors 32-39)
	// Each group has access bits [C1 C2 C3]. In this code C1 is MSB and C3 is LSB.
	// The four CX bits are stored together in a nible cx and an inverted nible cx_.
    uint8_t c1, c2, c3;		// Nibbles
    uint8_t c1_, c2_, c3_;		// Inverted nibbles
    bool invertedError = false;		// True if one of the inverted nibbles did not match
    uint8_t g[4];				// Access bits for each of the four groups.
    uint8_t group;				// 0-3 - active group for access bits
    bool firstInGroup;		// True for the first block dumped in the group

	// Determine position and size of sector.
	if (sector < 32) { // Sectors 0..31 has 4 blocks each
		no_of_blocks = 4;
		firstBlock = sector * no_of_blocks;
	}
	else if (sector < 40) { // Sectors 32-39 has 16 blocks each
		no_of_blocks = 16;
		firstBlock = 128 + (sector - 32) * no_of_blocks;
	}
	else { // Illegal input, no MIFARE Classic PICC has more than 40 sectors.
		return;
	}

	// Dump blocks, highest address first.
    uint8_t byteCount;
    uint8_t buffer[18];
    uint8_t blockAddr;
	isSectorTrailer = true;
	for (int8_t blockOffset = no_of_blocks - 1; blockOffset >= 0; blockOffset--) {
		blockAddr = firstBlock + blockOffset;
		// Sector number - only on first line
		if (isSectorTrailer) {
			if(sector < 10)
                serial_print("   "); // Pad with spaces
			else
                serial_print("  "); // Pad with spaces
			serial_print_f(sector, DEC);
            serial_print("   ");
		}
		else {
            serial_print("       ");
		}
        // Block number
		if(blockAddr < 10)
            serial_print("   "); // Pad with spaces
		else {
			if(blockAddr < 100)
                serial_print("  "); // Pad with spaces
			else
                serial_print(" "); // Pad with spaces
		}
		serial_print_f(blockAddr, DEC);
        serial_print("  ");
		// Establish encrypted communications before reading the first block
		if (isSectorTrailer) {
			status = PCD_Authenticate(PICC_CMD_MF_AUTH_KEY_A, firstBlock, key, uid);
			if (status != STATUS_OK) {
                serial_print("PCD_Authenticate() failed: ");
				serial_println(GetStatusCodeName(status));
				return;
			}
		}
		// Read block
		byteCount = sizeof(buffer);
		status = MIFARE_Read(blockAddr, buffer, &byteCount);
		if (status != STATUS_OK) {
            serial_print("MIFARE_Read() failed: ");
			serial_println(GetStatusCodeName(status));
			continue;
		}
		// Dump data
        for (uint8_t index = 0; index < 16; index++) {
			if(buffer[index] < 0x10)
                serial_print(" 0");
			else
                serial_print(" ");
			serial_print_f(buffer[index], HEX);
			if ((index % 4) == 3) {
                serial_print(" ");
			}
		}
		// Parse sector trailer data
		if (isSectorTrailer) {
			c1  = buffer[7] >> 4;
			c2  = buffer[8] & 0xF;
			c3  = buffer[8] >> 4;
			c1_ = buffer[6] & 0xF;
			c2_ = buffer[6] >> 4;
			c3_ = buffer[7] & 0xF;
            invertedError = (c1 != (~c1_ & 0xF)) || (c2 != (~c2_ & 0xF)) || (c3 != (~c3_ & 0xF));
            g[0] = ((c1 & 1) << 2) | ((c2 & 1) << 1) | ((c3 & 1) << 0);
            g[1] = ((c1 & 2) << 1) | ((c2 & 2) << 0) | ((c3 & 2) >> 1);
            g[2] = ((c1 & 4) << 0) | ((c2 & 4) >> 1) | ((c3 & 4) >> 2);
            g[3] = ((c1 & 8) >> 1) | ((c2 & 8) >> 2) | ((c3 & 8) >> 3);
			isSectorTrailer = false;
		}

		// Which access group is this block in?
		if (no_of_blocks == 4) {
			group = blockOffset;
            firstInGroup = true;
		}
		else {
			group = blockOffset / 5;
            firstInGroup = (group == 3) || (group != (blockOffset + 1) / 5);
		}

		if (firstInGroup) {
			// Print access bits
            serial_print(" [ ");
            serial_print_f((g[group] >> 2) & 1, DEC); serial_print(" ");
            serial_print_f((g[group] >> 1) & 1, DEC); serial_print(" ");
			serial_print_f((g[group] >> 0) & 1, DEC);
            serial_print(" ] ");
			if (invertedError) {
                serial_print(" Inverted access bits did not match! ");
			}
		}

		if (group != 3 && (g[group] == 1 || g[group] == 6)) { // Not a sector trailer, a value block
			long value = ((long)((buffer[3])<<24)) | ((long)((buffer[2])<<16)) | ((long)((buffer[1])<<8)) | ((long)(buffer[0]));
            serial_print(" Value=0x"); serial_print_f(value, HEX);
            serial_print(" Adr=0x"); serial_print_f(buffer[12], HEX);
		}
		serial_println("");
	}
} // End PICC_DumpMifareClassicSectorToSerial()

/**
 * Dumps memory contents of a MIFARE Ultralight PICC.
 */
void PICC_DumpMifareUltralightToSerial() {
    uint8_t status;
    uint8_t byteCount;
    uint8_t buffer[18];
    uint8_t i;

    serial_println("Page  0  1  2  3");
	// Try the mpages of the original Ultralight. Ultralight C has more pages.
    for (uint8_t page = 0; page < 16; page +=4) { // Read returns data for 4 pages at a time.
		// Read pages
		byteCount = sizeof(buffer);
		status = MIFARE_Read(page, buffer, &byteCount);
		if (status != STATUS_OK) {
            serial_print("MIFARE_Read() failed: ");
			serial_println(GetStatusCodeName(status));
			break;
		}
		// Dump data
        for (uint8_t offset = 0; offset < 4; offset++) {
			i = page + offset;
			if(i < 10)
                serial_print("  "); // Pad with spaces
			else
                serial_print(" "); // Pad with spaces
			serial_print_f(i, DEC);
            serial_print("  ");
            for (uint8_t index = 0; index < 4; index++) {
				i = 4 * offset + index;
				if(buffer[i] < 0x10)
                    serial_print(" 0");
				else
                    serial_print(" ");
				serial_print_f(buffer[i], HEX);
			}
			serial_println("");
		}
	}
} // End PICC_DumpMifareUltralightToSerial()

/**
 * Calculates the bit pattern needed for the specified access bits. In the [C1 C2 C3] tupples C1 is MSB (=4) and C3 is LSB (=1).
 */
void MIFARE_SetAccessBits(	uint8_t *accessBitBuffer,	///< Pointer to byte 6, 7 and 8 in the sector trailer. Bytes [0..2] will be set.
                                    uint8_t g0,				///< Access bits [C1 C2 C3] for block 0 (for sectors 0-31) or blocks 0-4 (for sectors 32-39)
                                    uint8_t g1,				///< Access bits C1 C2 C3] for block 1 (for sectors 0-31) or blocks 5-9 (for sectors 32-39)
                                    uint8_t g2,				///< Access bits C1 C2 C3] for block 2 (for sectors 0-31) or blocks 10-14 (for sectors 32-39)
                                    uint8_t g3					///< Access bits C1 C2 C3] for the sector trailer, block 3 (for sectors 0-31) or block 15 (for sectors 32-39)
								) {
    uint8_t c1 = ((g3 & 4) << 1) | ((g2 & 4) << 0) | ((g1 & 4) >> 1) | ((g0 & 4) >> 2);
    uint8_t c2 = ((g3 & 2) << 2) | ((g2 & 2) << 1) | ((g1 & 2) << 0) | ((g0 & 2) >> 1);
    uint8_t c3 = ((g3 & 1) << 3) | ((g2 & 1) << 2) | ((g1 & 1) << 1) | ((g0 & 1) << 0);

	accessBitBuffer[0] = (~c2 & 0xF) << 4 | (~c1 & 0xF);
	accessBitBuffer[1] =          c1 << 4 | (~c3 & 0xF);
	accessBitBuffer[2] =          c3 << 4 | c2;
} // End MIFARE_SetAccessBits()


/**
 * Performs the "magic sequence" needed to get Chinese UID changeable
 * Mifare cards to allow writing to sector 0, where the card UID is stored.
 *
 * Note that you do not need to have selected the card through REQA or WUPA,
 * this sequence works immediately when the card is in the reader vicinity.
 * This means you can use this method even on "bricked" cards that your reader does
 * not recognise anymore (see MIFARE_UnbrickUidSector).
 *
 * Of course with non-bricked devices, you're free to select them before calling this function.
 */
bool MIFARE_OpenUidBackdoor(bool logErrors) {
	// Magic sequence:
	// > 50 00 57 CD (HALT + CRC)
	// > 40 (7 bits only)
	// < A (4 bits only)
	// > 43
	// < A (4 bits only)
	// Then you can write to sector 0 without authenticating

	PICC_HaltA(); // 50 00 57 CD

    uint8_t cmd = 0x40;
    uint8_t validBits = 7; /* Our command is only 7 bits. After receiving card response,
						  this will contain amount of valid response bits. */
    uint8_t response[32]; // Card's response is written here
    uint8_t received;
    uint8_t status = PCD_TransceiveData(&cmd, (uint8_t)1, response, &received, &validBits, (uint8_t)0, false); // 40
	if(status != STATUS_OK) {
		if (logErrors) {
            serial_println("Card did not respond to 0x40 after HALT command. Are you sure it is a UID changeable one?");
            serial_print("Error name: ");
			serial_println(GetStatusCodeName(status));
		}
		return false;
	}
	if (received != 1 || response[0] != 0x0A) {
		if (logErrors) {
            serial_print("Got bad response on backdoor 0x40 command: ");
			serial_print_f(response[0], HEX);
            serial_print(" (");
			serial_print_f(validBits, DEC);
            serial_print(" valid bits)\r\n");
		}
		return false;
	}

	cmd = 0x43;
	validBits = 8;
    status = PCD_TransceiveData(&cmd, (uint8_t)1, response, &received, &validBits, (uint8_t)0, false); // 43
	if(status != STATUS_OK) {
		if(logErrors) {
            serial_println("Error in communication at command 0x43, after successfully executing 0x40");
            serial_print("Error name: ");
			serial_println(GetStatusCodeName(status));
		}
		return false;
	}
	if (received != 1 || response[0] != 0x0A) {
		if (logErrors) {
            serial_print("Got bad response on backdoor 0x43 command: ");
			serial_print_f(response[0], HEX);
            serial_print(" (");
			serial_print_f(validBits, DEC);
            serial_print(" valid bits)\r\n");
		}
		return false;
	}

	// You can now write to sector 0 without authenticating!
	return true;
} // End MIFARE_OpenUidBackdoor()

/**
 * note: Only for specialized cards that allow changing block 0 (these are not normal/typical cards)
 *
 * Reads entire block 0, including all manufacturer data, and overwrites
 * that block with the new UID, a freshly calculated BCC, and the original
 * manufacturer data.
 *
 * It assumes a default KEY A of 0xFFFFFFFFFFFF.
 * Make sure to have selected the card before this function is called.
 */
bool MIFARE_SetUid(uint8_t *newUid, uint8_t uidSize, bool logErrors) {

	// UID + BCC byte can not be larger than 16 together
	if (!newUid || !uidSize || uidSize > 15) {
		if (logErrors) {
            serial_println("New UID buffer empty, size 0, or size > 15 given");
		}
		return false;
	}

	// Authenticate for reading
    MIFARE_Key key = {{0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF}};
    Uid original_id;
    uint8_t status = PCD_Authenticate(PICC_CMD_MF_AUTH_KEY_A, (uint8_t)1, &key, &original_id);
	if (status != STATUS_OK) {

		if (status == STATUS_TIMEOUT) {
			// We get a read timeout if no card is selected yet, so let's select one

			// Wake the card up again if sleeping
//			  byte atqa_answer[2];
//			  byte atqa_size = 2;
//			  PICC_WakeupA(atqa_answer, &atqa_size);

			if (!PICC_IsNewCardPresent() || !PICC_ReadCardSerial(&original_id)) {
                serial_println("No card was previously selected, and none are available. Failed to set UID.");
				return false;
			}

            status = PCD_Authenticate(PICC_CMD_MF_AUTH_KEY_A, (uint8_t)1, &key, &original_id);
			if (status != STATUS_OK) {
				// We tried, time to give up
				if (logErrors) {
                    serial_println("Failed to authenticate to card for reading, could not set UID: ");
					serial_println(GetStatusCodeName(status));
				}
				return false;
			}
		}
		else {
			if (logErrors) {
                serial_print("PCD_Authenticate() failed: ");
				serial_println(GetStatusCodeName(status));
			}
			return false;
		}
	}

	// Read block 0
    uint8_t block0_buffer[18];
    uint8_t byteCount = sizeof(block0_buffer);
    status = MIFARE_Read((uint8_t)0, block0_buffer, &byteCount);
	if (status != STATUS_OK) {
		if (logErrors) {
            serial_print("MIFARE_Read() failed: ");
			serial_println(GetStatusCodeName(status));
            serial_println("Are you sure your KEY A for sector 0 is 0xFFFFFFFFFFFF?");
		}
		return false;
	}

	// Write new UID to the data we just read, and calculate BCC byte
    uint8_t bcc = 0;
	for (int i = 0; i < uidSize; i++) {
		block0_buffer[i] = newUid[i];
		bcc ^= newUid[i];
	}

	// Write BCC byte to buffer
	block0_buffer[uidSize] = bcc;

	// Stop encrypted traffic so we can send raw bytes
	PCD_StopCrypto1();

	// Activate UID backdoor
	if (!MIFARE_OpenUidBackdoor(logErrors)) {
		if (logErrors) {
            serial_println("Activating the UID backdoor failed.");
		}
		return false;
	}

	// Write modified block 0 back to card
    status = MIFARE_Write((uint8_t)0, block0_buffer, (uint8_t)16);
	if (status != STATUS_OK) {
		if (logErrors) {
            serial_print("MIFARE_Write() failed: ");
			serial_println(GetStatusCodeName(status));
		}
		return false;
	}

	// Wake the card up again
    uint8_t atqa_answer[2];
    uint8_t atqa_size = 2;
	PICC_WakeupA(atqa_answer, &atqa_size);

	return true;
}

/**
 * Resets entire sector 0 to zeroes, so the card can be read again by readers.
 */
bool MIFARE_UnbrickUidSector(bool logErrors) {
	MIFARE_OpenUidBackdoor(logErrors);

    uint8_t block0_buffer[] = {0x01, 0x02, 0x03, 0x04, 0x04, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00};

	// Write modified block 0 back to card
    uint8_t status = MIFARE_Write((uint8_t)0, block0_buffer, (uint8_t)16);
	if (status != STATUS_OK) {
		if (logErrors) {
            serial_print("MIFARE_Write() failed: ");
			serial_println(GetStatusCodeName(status));
		}
		return false;
	}
	return true;
}

/////////////////////////////////////////////////////////////////////////////////////
// Convenience functions - does not add extra functionality
/////////////////////////////////////////////////////////////////////////////////////

/**
 * Returns true if a PICC responds to PICC_CMD_REQA.
 * Only "new" cards in state IDLE are invited. Sleeping cards in state HALT are ignored.
 *
 * @return bool
 */
bool PICC_IsNewCardPresent() {
    uint8_t bufferATQA[2];
    uint8_t bufferSize = sizeof(bufferATQA);
    uint8_t result = PICC_RequestA(bufferATQA, &bufferSize);
	return (result == STATUS_OK || result == STATUS_COLLISION);
} // End PICC_IsNewCardPresent()

/**
 * Simple wrapper around PICC_Select.
 * Returns true if a UID could be read.
 * Remember to call PICC_IsNewCardPresent(), PICC_RequestA() or PICC_WakeupA() first.
 * The read UID is available in the global variable g_mfrc._uid).
 *
 * @return true if a UID was present and it was copied into uid, false if not. uid will be unchanged if not successful.
 * the UID passed in will be a COPY of the last UID scanned (if it exists), you do not need to free or de-allocate it.
 * don't use it unless the function returns true.
 */
bool PICC_ReadCardSerial(Uid* uid)
{
    assert(uid);
    if (!uid)
        return false;

    return PICC_Select(uid, 0) == STATUS_OK;
}
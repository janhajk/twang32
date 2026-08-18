// A very simple implementation of the MPU-6050 limited to the
// TWANG requirements
// I reused the function names to make it compatible
// B. Dring 2/2018
// Updated to store connected state and other data in class object
// JS 06/2025

class Twang_MPU
{
public:
	Twang_MPU(uint16_t address);
	void initialize();
	bool getMotion6();
	bool getMotion6(int16_t *xAccel, int16_t *yAccel, int16_t *zAccel, int16_t *xGyro, int16_t *yGyro, int16_t *zGyro);
	bool testConnection();

	uint16_t devAddr;
	bool connected; // cached connected value, updated by testConnection and on error
	int16_t ax, ay, az;
	int16_t gx, gy, gz;

	static const uint16_t MPU_ADDR_DEFAULT = 0x68;
	static const uint16_t MPU_ADDR_ALTERNATIVE = 0x69; // if AD0 pin is HIGH

private:
	static const uint8_t PWR_MGMT_1 = 0x6B;
	static const uint8_t MPU_DATA_REG_START = 0x3B;
	static const uint8_t MPU_DATA_LEN = 14;
	static const uint8_t MPU_DATA_WHO_AM_I = 0x75;
};

Twang_MPU::Twang_MPU(uint16_t address)
{
	this->devAddr = address;
	this->connected = false;
}

void Twang_MPU::initialize()
{
	Wire.beginTransmission(devAddr);
	Wire.write(PWR_MGMT_1); // PWR_MGMT_1 register
	Wire.write(0);			// set to zero (wakes up the MPU-6050)
	Wire.endTransmission(true);
}

// returns connected state
bool Twang_MPU::testConnection()
{
	Wire.beginTransmission(devAddr);
	Wire.write(MPU_DATA_WHO_AM_I);
	Wire.endTransmission(false);
	Wire.requestFrom(devAddr, (uint8_t)1, true); // read the whole MPU data section
	// NOTE: The WHO_AM_I register only returns the upper 6 bits of the address
	// therefore it will not correctly return the address if AD0 is HIGH, but
	// always the default value 0x68
	// see documentation section 4.32, "Register 117 – Who Am I" (p.45): 
	// https://invensense.tdk.com/wp-content/uploads/2015/02/MPU-6000-Register-Map1.pdf
	int addr = Wire.read();
	this->connected = (addr == MPU_ADDR_DEFAULT);
	return this->connected;
}

// returns connected state, stores motion values internally
bool Twang_MPU::getMotion6()
{
	return getMotion6(&ax, &ay, &az, &gx, &gy, &gz);
}

// returns connected state, stores motion values in passed variables
bool Twang_MPU::getMotion6(int16_t *xAccel, int16_t *yAccel, int16_t *zAccel, int16_t *xGyro, int16_t *yGyro, int16_t *zGyro)
{
	Wire.beginTransmission(devAddr);
	Wire.write(MPU_DATA_REG_START); // starting with register 0x3B (ACCEL_XOUT_H)
	Wire.endTransmission(false);
	Wire.requestFrom(devAddr, MPU_DATA_LEN, true); // read the whole MPU data section
	unsigned long timeout = millis();
	const unsigned long TIMEOUT_MS = 5;
	while (Wire.available() < MPU_DATA_LEN)
	{
		if (millis() - timeout > TIMEOUT_MS)
		{
			this->connected = false;
			return false;
		}
		delayMicroseconds(100);
	}
	*xAccel = Wire.read() << 8 | Wire.read(); // x Accel
	*yAccel = Wire.read() << 8 | Wire.read(); // y Accel
	*zAccel = Wire.read() << 8 | Wire.read(); // z Accel
	Wire.read();
	Wire.read();							 // Temperature..not used, but need to read it
	*xGyro = Wire.read() << 8 | Wire.read(); // x Gyro
	*yGyro = Wire.read() << 8 | Wire.read(); // y Gyro
	*zGyro = Wire.read() << 8 | Wire.read(); // z Gyro
	return true;
}

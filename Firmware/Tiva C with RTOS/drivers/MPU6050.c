#include <stdint.h>
#include <stdbool.h>
#include <math.h>
#include <stdio.h>

#include "MPU6050.h"
#include "debug.h"
#include "FreeRTOS.h"

#include "inc/tm4c123gh6pm.h"
#include "inc/hw_memmap.h"
#include "driverlib/gpio.h"
#include "driverlib/pin_map.h"
#include "driverlib/i2c.h"
#include "driverlib/sysctl.h"
#include "driverlib/timer.h"
#include "driverlib/interrupt.h"

// Comment out to restrict roll to ±90deg instead - please read: http://www.freescale.com/files/sensors/doc/app_note/AN3461.pdf
#define RESTRICT_PITCH

#define MPU6050_SMPLRT_DIV                  0x19
#define MPU6050_INT_PIN_CFG                 0x37
#define MPU6050_ACCEL_XOUT_H                0x3B
#define MPU6050_GYRO_XOUT_H                 0x43
#define MPU6050_PWR_MGMT_1                  0x6B
#define MPU6050_WHO_AM_I                    0x75

#define MPU6050_ADDRESS                     0x68
#define MPU6050_WHO_AM_I_ID                 0x68

// Scale factor for +-2000deg/s and +-8g - see datasheet:
#define MPU6050_GYRO_SCALE_FACTOR_2000      16.4f
#define MPU6050_ACC_SCALE_FACTOR_8          4096.0f

#define RAD_TO_DEG                          (180/3.14159)
#define dt_update                           (1.0/FREQ_UPDATE)

static uint32_t ui32Period;

static float f_Q_angle = 0.001f;
static float f_Q_bias = 0.003f;
static float f_R_measure = 0.03f;
static float f_angle = 0.0f; // Reset the angle
static float f_bias = 0.0f; // Reset bias
static float f_rate = 0.0f;
static float f_P[2][2];

static float f_angleX_;
static float f_angleY_;

double gyroXangle, gyroYangle; // Angle calculate using the gyro only
double compAngleX, compAngleY; // Calculated angle using a complementary filter
double kalAngleX, kalAngleY; // Calculated angle using a Kalman filter

static void I2C_MPU6050_Init(void);
static void I2C_Write(uint8_t addr, uint8_t regAddr, uint8_t data);
static void I2C_Write_Data(uint8_t addr, uint8_t regAddr, uint8_t *date, uint8_t length);
static uint8_t I2C_Read(uint8_t addr, uint8_t regAddr);
static void I2C_Read_Data(uint8_t addr, uint8_t regAddr, uint8_t *data, uint8_t length);



static void MPU6050_First_Get_Data(void);

static void Kalman_Filter_Init(void);
static float Kalman_Get_Angle(float newAngle, float newRate, float dt);
//static void Kalman_Filter_Process(void);

static void delay (uint32_t delay){
    SysCtlDelay(delay);
}

static void I2C_MPU6050_Init(void) {
    SysCtlPeripheralEnable(MPU6050_I2C); // Enable I2C1 peripheral
    delay(2); // Insert a few cycles after enabling the peripheral to allow the clock to be fully activated
    SysCtlPeripheralEnable(MPU6050_I2C_GPIO); // Enable GPIOA peripheral
    delay(2); // Insert a few cycles after enabling the peripheral to allow the clock to be fully activated

    // Use alternate function
    GPIOPinConfigure(MPU6050_I2C_SCL);
    GPIOPinConfigure(MPU6050_I2C_SDA);

    GPIOPinTypeI2CSCL(MPU6050_I2C_GPIO_BASE, MPU6050_GPIO_SCL); // Use pin with I2C SCL peripheral
    GPIOPinTypeI2C(MPU6050_I2C_GPIO_BASE, MPU6050_GPIO_SDA); // Use pin with I2C peripheral

    I2CMasterInitExpClk(MPU6050_I2C_BASE, SysCtlClockGet(), false); // Enable and set frequency to 400 kHz

    delay(2); // Insert a few cycles after enabling the I2C to allow the clock to be fully activated
}

static void I2C_Write(uint8_t addr, uint8_t regAddr, uint8_t data) {
    I2C_Write_Data(addr, regAddr, &data, 1);
}

static void I2C_Write_Data(uint8_t addr, uint8_t regAddr, uint8_t *data, uint8_t length) {
    I2CMasterSlaveAddrSet(MPU6050_I2C_BASE, addr, false); // Set to write mode

    I2CMasterDataPut(MPU6050_I2C_BASE, regAddr); // Place address into data register
    I2CMasterControl(MPU6050_I2C_BASE, I2C_MASTER_CMD_BURST_SEND_START); // Send start condition
    while (I2CMasterBusy(MPU6050_I2C_BASE)); // Wait until transfer is done

    uint8_t i = 0;
    for (i = 0; i < length - 1; i++) {
        I2CMasterDataPut(MPU6050_I2C_BASE, data[i]); // Place data into data register
        I2CMasterControl(MPU6050_I2C_BASE, I2C_MASTER_CMD_BURST_SEND_CONT); // Send continues condition
        while (I2CMasterBusy(MPU6050_I2C_BASE)); // Wait until transfer is done
    }

    I2CMasterDataPut(MPU6050_I2C_BASE, data[length - 1]); // Place data into data register
    I2CMasterControl(MPU6050_I2C_BASE, I2C_MASTER_CMD_BURST_SEND_FINISH); // Send finish condition
    while (I2CMasterBusy(MPU6050_I2C_BASE)); // Wait until transfer is done
}

static uint8_t I2C_Read(uint8_t addr, uint8_t regAddr) {
    I2CMasterSlaveAddrSet(MPU6050_I2C_BASE, addr, false); // Set to write mode

    I2CMasterDataPut(MPU6050_I2C_BASE, regAddr); // Place address into data register
    I2CMasterControl(MPU6050_I2C_BASE, I2C_MASTER_CMD_SINGLE_SEND); // Send data
    while (I2CMasterBusy(MPU6050_I2C_BASE)); // Wait until transfer is done

    I2CMasterSlaveAddrSet(MPU6050_I2C_BASE, addr, true); // Set to read mode

    I2CMasterControl(MPU6050_I2C_BASE, I2C_MASTER_CMD_SINGLE_RECEIVE); // Tell master to read data
    while (I2CMasterBusy(MPU6050_I2C_BASE)); // Wait until transfer is done
    return I2CMasterDataGet(MPU6050_I2C_BASE); // Read data
}

static void I2C_Read_Data(uint8_t addr, uint8_t regAddr, uint8_t *data, uint8_t length) {
    I2CMasterSlaveAddrSet(MPU6050_I2C_BASE, addr, false); // Set to write mode

    I2CMasterDataPut(MPU6050_I2C_BASE, regAddr); // Place address into data register
    I2CMasterControl(MPU6050_I2C_BASE, I2C_MASTER_CMD_SINGLE_SEND); // Send data
    while (I2CMasterBusy(MPU6050_I2C_BASE)); // Wait until transfer is done

    I2CMasterSlaveAddrSet(MPU6050_I2C_BASE, addr, true); // Set to read mode

    I2CMasterControl(MPU6050_I2C_BASE, I2C_MASTER_CMD_BURST_RECEIVE_START); // Send start condition
    while (I2CMasterBusy(MPU6050_I2C_BASE)); // Wait until transfer is done

    data[0] = I2CMasterDataGet(MPU6050_I2C_BASE); // Place data into data register
    uint8_t i = 1;
    for (i = 1; i < length - 1; i++) {
        I2CMasterControl(MPU6050_I2C_BASE, I2C_MASTER_CMD_BURST_RECEIVE_CONT); // Send continues condition
        while (I2CMasterBusy(MPU6050_I2C_BASE)); // Wait until transfer is done
        data[i] = I2CMasterDataGet(MPU6050_I2C_BASE); // Place data into data register
    }

    I2CMasterControl(MPU6050_I2C_BASE, I2C_MASTER_CMD_BURST_RECEIVE_FINISH); // Send finish condition
    while (I2CMasterBusy(MPU6050_I2C_BASE)); // Wait until transfer is done
    data[length - 1] = I2CMasterDataGet(MPU6050_I2C_BASE); // Place data into data register
}

static int8_t xx = 0;

void mpu_service_loop(void){
    // Clear the timer interrupt
while(1)
{
    Kalman_Filter_Process();
//    vTaskDelay(100);
}
}



void MPU6050_Init(void) {
        SysCtlPeripheralEnable(SYSCTL_PERIPH_GPIOF); //enable GPIO port for LED
        GPIOPinTypeGPIOOutput(GPIO_PORTF_BASE, GPIO_PIN_1); //enable pin for LED PF2
        SysCtlPeripheralEnable(SYSCTL_PERIPH_GPIOF);
        GPIOPinTypeGPIOOutput(GPIO_PORTF_BASE, GPIO_PIN_3|GPIO_PIN_2);
        GPIOPinTypeGPIOOutput(GPIO_PORTF_BASE, GPIO_PIN_1); //enable pin for LED PF2
    I2C_MPU6050_Init();
    Kalman_Filter_Init();

    uint8_t i2cBuffer[5]; // Buffer for I2C data
    i2cBuffer[0] = I2C_Read(MPU6050_ADDRESS, MPU6050_WHO_AM_I);
    I2C_Write(MPU6050_ADDRESS, MPU6050_PWR_MGMT_1, (1 << 7)); // Reset device, this resets all internal registers to their default values
    SysCtlDelay(SysCtlClockGet()/100);
    while (I2C_Read(MPU6050_ADDRESS, MPU6050_PWR_MGMT_1) & (1 << 7)) {
    // Wait for the bit to clear
    };
    SysCtlDelay(SysCtlClockGet()/100);
    I2C_Write(MPU6050_ADDRESS, MPU6050_PWR_MGMT_1, (1 << 3) | (1 << 0)); // Disable sleep mode, disable temperature sensor and use PLL as clock reference

    i2cBuffer[0] = 0; // Set the sample rate to 1kHz - 1kHz/(1+0) = 1kHz
    i2cBuffer[1] = 0x03; // Disable FSYNC and set 41 Hz Gyro filtering, 1 KHz sampling
    i2cBuffer[2] = 3 << 3; // Set Gyro Full Scale Range to +-2000deg/s
    i2cBuffer[3] = 2 << 3; // Set Accelerometer Full Scale Range to +-8g
    i2cBuffer[4] = 0x03; // 41 Hz Acc filtering
    I2C_Write_Data(MPU6050_ADDRESS, MPU6050_SMPLRT_DIV, i2cBuffer, 5); // Write to all five registers at once


    /* Enable Raw Data Ready Interrupt on INT pin */
    i2cBuffer[0] = (1 << 5) | (1 << 4); // Enable LATCH_INT_EN and INT_ANYRD_2CLEAR
    // When this bit is equal to 1, the INT pin is held high until the interrupt is cleared
    // When this bit is equal to 1, interrupt status is cleared if any read operation is performed
    i2cBuffer[1] = (1 << 0);            // Enable RAW_RDY_EN - When set to 1, Enable Raw Sensor Data Ready interrupt to propagate to interrupt pin
    I2C_Write_Data(MPU6050_ADDRESS, MPU6050_INT_PIN_CFG, i2cBuffer, 2); // Write to both registers at once

    delay(SysCtlClockGet()/3);

//    MPU6050_First_Get_Data();
    delay(SysCtlClockGet()/30);
}

void MPU6050_Get_Accel_Raw(int16_t * bufData) {
    uint8_t buf[6];
    I2C_Read_Data(MPU6050_ADDRESS, MPU6050_ACCEL_XOUT_H, buf, 6); // Note that we can't write directly into MPU6050_t, because of endian conflict. So it has to be done manually

    bufData[0] = (buf[0] << 8) | buf[1];    //Accel X
    bufData[1] = (buf[2] << 8) | buf[3];    //Accel Y
    bufData[2] = (buf[4] << 8) | buf[5];    //Accel Z
}

void MPU6050_Get_Gyro_Raw(int16_t * bufData) {
    uint8_t buf[6];
    I2C_Read_Data(MPU6050_ADDRESS, MPU6050_GYRO_XOUT_H, buf, 6); // Note that we can't write directly into MPU6050_t, because of endian conflict. So it has to be done manually

    bufData[0] = (buf[0] << 8) | buf[1];    //Gyro X
    bufData[1] = (buf[2] << 8) | buf[3];    //Gyro Y
    bufData[2] = (buf[4] << 8) | buf[5];    //Gyro Z
}

static void MPU6050_First_Get_Data(void){
    int16_t i16_Accel[3];
    MPU6050_Get_Accel_Raw(i16_Accel);

    double accX, accY, accZ;
    accX = i16_Accel[0];
    accY = i16_Accel[1];
    accZ = i16_Accel[2];

#ifdef RESTRICT_PITCH // Eq. 25 and 26
    double roll  = atan2(accY, accZ) * RAD_TO_DEG;
    double pitch = atan(-accX / sqrt(accY * accY + accZ * accZ)) * RAD_TO_DEG;
#else // Eq. 28 and 29
    double roll  = atan(accY / sqrt(accX * accX + accZ * accZ)) * RAD_TO_DEG;
    double pitch = atan2(-accX, accZ) * RAD_TO_DEG;
#endif

    f_angleX_ = roll;   // Set starting angle
    f_angleY_ = pitch;
    gyroXangle = roll;
    gyroYangle = pitch;
    compAngleX = roll;
    compAngleY = pitch;
}

void Kalman_Filter_Process(void){
    int16_t i16_Accel[3];
    int16_t i16_Gyro[3];
    double accX, accY, accZ;
    double gyroX, gyroY, gyroZ;

    MPU6050_Get_Accel_Raw(i16_Accel);
    MPU6050_Get_Gyro_Raw(i16_Gyro);

    accX = i16_Accel[0];
    accY = i16_Accel[1];
    accZ = i16_Accel[2];

    gyroX = i16_Gyro[0];
    gyroY = i16_Gyro[1];
    gyroZ = i16_Gyro[2];

    // Source: http://www.freescale.com/files/sensors/doc/app_note/AN3461.pdf eq. 25 and eq. 26
    // atan2 outputs the value of -π to π (radians) - see http://en.wikipedia.org/wiki/Atan2
    // It is then converted from radians to degrees
  #ifdef RESTRICT_PITCH // Eq. 25 and 26
    double roll  = atan2(accY, accZ) * RAD_TO_DEG;
    double pitch = atan(-accX / sqrt(accY * accY + accZ * accZ)) * RAD_TO_DEG;
  #else // Eq. 28 and 29
    double roll  = atan(accY / sqrt(accX * accX + accZ * accZ)) * RAD_TO_DEG;
    double pitch = atan2(-accX, accZ) * RAD_TO_DEG;
  #endif

    double gyroXrate = gyroX / 131.0; // Convert to deg/s
    double gyroYrate = gyroY / 131.0; // Convert to deg/s

#ifdef RESTRICT_PITCH
    // This fixes the transition problem when the accelerometer angle jumps between -180 and 180 degrees
    f_angle = f_angleX_;
    if ((roll < -90 && kalAngleX > 90) || (roll > 90 && kalAngleX < -90)) {
        f_angle = roll;
        compAngleX = roll;
        kalAngleX = roll;
        gyroXangle = roll;
    }
    else
    {
        kalAngleX = Kalman_Get_Angle(roll, gyroXrate, dt_update); // Calculate the angle using a Kalman filter
        f_angleX_ = kalAngleX;
    }

    f_angle = f_angleY_;
    if (abs(kalAngleX) > 90)
    {
        gyroYrate = -gyroYrate; // Invert rate, so it fits the restriced accelerometer reading
    }
    kalAngleY = Kalman_Get_Angle(pitch, gyroYrate, dt_update);
    f_angleY_ = gyroYrate;
#else
    // This fixes the transition problem when the accelerometer angle jumps between -180 and 180 degrees
    f_angle = f_angleY_;
    if ((pitch < -90 && kalAngleY > 90) || (pitch > 90 && kalAngleY < -90)) {
        f_angle = pitch;
          compAngleY = pitch;
          kalAngleY = pitch;
          gyroYangle = pitch;
    }
    else
    {
        kalAngleY = kalmanY.getAngle(pitch, gyroYrate, dt_update); // Calculate the angle using a Kalman filter
        f_angleY_ = kalAngleY;
    }

    f_angle = f_angleX_;
    if (abs(kalAngleY) > 90)
        gyroXrate = -gyroXrate; // Invert rate, so it fits the restriced accelerometer reading
    kalAngleX = kalmanX.getAngle(roll, gyroXrate, dt_update); // Calculate the angle using a Kalman filter
    f_angleX_ = kalAngleX;
#endif

    gyroXangle += gyroXrate * dt_update; // Calculate gyro angle without any filter
    gyroYangle += gyroYrate * dt_update;
    //gyroXangle += kalmanX.getRate() * dt; // Calculate gyro angle using the unbiased rate
    //gyroYangle += kalmanY.getRate() * dt;

    compAngleX = 0.99 * (compAngleX + gyroXrate * dt_update) + 0.01 * roll; // Calculate the angle using a Complimentary filter
    compAngleY = 0.99 * (compAngleY + gyroYrate * dt_update) + 0.01 * pitch;

    // Reset the gyro angle when it has drifted too much
    if (gyroXangle < -180 || gyroXangle > 180)
        gyroXangle = kalAngleX;
    if (gyroYangle < -180 || gyroYangle > 180)
        gyroYangle = kalAngleY;
}

void MPU6050_Kalman_Angle(double * d_angle){
    d_angle[0] = kalAngleX;
    d_angle[1] = kalAngleY;
}

void MPU6050_Complimentary_Angle(float * d_angle){
    d_angle[0] = (float) compAngleX;
    d_angle[1] = (float)compAngleY;
}

static void Kalman_Filter_Init(void) {
    /* We will set the variables like so, these can also be tuned by the user */
    f_Q_angle = 0.001f;
    f_Q_bias = 0.003f;
    f_R_measure = 0.03f;

    f_angle = 0.0f; // Reset the angle
    f_bias = 0.0f; // Reset bias

    f_P[0][0] = 0.0f; // Since we assume that the bias is 0 and we know the starting angle (use setAngle), the error covariance matrix is set like so - see: http://en.wikipedia.org/wiki/Kalman_filter#Example_application.2C_technical
    f_P[0][1] = 0.0f;
    f_P[1][0] = 0.0f;
    f_P[1][1] = 0.0f;
}

// The angle should be in degrees and the rate should be in degrees per second and the delta time in seconds
static float Kalman_Get_Angle(float newAngle, float newRate, float dt) {
    // KasBot V2  -  Kalman filter module - http://www.x-firm.com/?page_id=145
    // Modified by Kristian Lauszus
    // See my blog post for more information: http://blog.tkjelectronics.dk/2012/09/a-practical-approach-to-kalman-filter-and-how-to-implement-it

    // Discrete Kalman filter time update equations - Time Update ("Predict")
    // Update xhat - Project the state ahead
    /* Step 1 */
    f_rate = newRate - f_bias;
    f_angle += dt * f_rate;

    // Update estimation error covariance - Project the error covariance ahead
    /* Step 2 */
    f_P[0][0] += dt * (dt*f_P[1][1] - f_P[0][1] - f_P[1][0] + f_Q_angle);
    f_P[0][1] -= dt * f_P[1][1];
    f_P[1][0] -= dt * f_P[1][1];
    f_P[1][1] += f_Q_bias * dt;

    // Discrete Kalman filter measurement update equations - Measurement Update ("Correct")
    // Calculate Kalman gain - Compute the Kalman gain
    /* Step 4 */
    float S = f_P[0][0] + f_R_measure; // Estimate error
    /* Step 5 */
    float K[2]; // Kalman gain - This is a 2x1 vector
    K[0] = f_P[0][0] / S;
    K[1] = f_P[1][0] / S;

    // Calculate angle and bias - Update estimate with measurement zk (newAngle)
    /* Step 3 */
    float y = newAngle - f_angle; // Angle difference
    /* Step 6 */
    f_angle += K[0] * y;
    f_bias += K[1] * y;

    // Calculate estimation error covariance - Update the error covariance
    /* Step 7 */
    float P00_temp = f_P[0][0];
    float P01_temp = f_P[0][1];

    f_P[0][0] -= K[0] * P00_temp;
    f_P[0][1] -= K[0] * P01_temp;
    f_P[1][0] -= K[1] * P00_temp;
    f_P[1][1] -= K[1] * P01_temp;

    return f_angle;
}

void Kalman_Set_Angle(float angle) {
    f_angle = angle;
}   // Used to set angle, this should be set as the starting angle

float Kalman_Get_Rate(void) {
    return f_rate;
}   // Return the unbiased rate

/* These are used to tune the Kalman filter */
void Kalman_Set_Qangle(float Q_angle) {
    f_Q_angle = Q_angle;
}

void Kalman_Set_Qbias(float Q_bias) {
    f_Q_bias = Q_bias;
}

void Kalman_Set_Rmeasure(float R_measure) {
    f_R_measure = R_measure;
}

float Kalman_Get_Qangle(void) {
    return f_Q_angle;
}

float Kalman_Get_Qbias(void) {
    return f_Q_bias;
}

float Kalman_Get_Rmeasure(void) {
    return f_R_measure;
}

xStatusTypeDef mpu_service_init(void)
{
    MPU6050_Init();
    xTaskCreate( mpu_service_loop, "MPU Get", configMINIMAL_STACK_SIZE, NULL, 1, NULL );

    return OK;
}
xStatusTypeDef mpu_service_start(void)
{
    return OK;
}


/* END OF FILE*/

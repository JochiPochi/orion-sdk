#include "GeolocateTelemetry.h"
#include "earthposition.h"
#include "earthrotation.h"
#include "mathutilities.h"
#include "WGS84.h"



/*!
 * Create a GeolocateTelemetry packet
 * \param pPkt receives the formatted packet
 * \param pGeo is the geo locate telemetry to encode
 */
void FormGeolocateTelemetry(OrionPkt_t *pPkt, const GeolocateTelemetry_t *pGeo)
{
    encodeGeolocateTelemetryCorePacketStructure(pPkt, &pGeo->base);

}// FormGeolocateTelemetry


/*!
 * Parse a GeolocateTelemetry packet
 * \param pPkt is the received packet packet
 * \param pGeo receives the parsed data, including locally constructed data
 * \return TRUE if the packet was successfully decoded
 */
BOOL DecodeGeolocateTelemetry(const OrionPkt_t *pPkt, GeolocateTelemetry_t *pGeo)
{
	// Only parse this packet if the ID and length look right
    if (decodeGeolocateTelemetryCorePacketStructure(pPkt, &pGeo->base))
	{
		stackAllocateDCM(tempDcm);
        float Pan, Tilt;

        // Date and time
        computeDateAndTimeFromWeekAndItow(pGeo->base.gpsWeek, pGeo->base.gpsITOW, pGeo->base.leapSeconds, &pGeo->Year, &pGeo->Month, &pGeo->Day, &pGeo->Hour, &pGeo->Minute, &pGeo->Second);

		// convert tilt from -180 to 180 into -270 to 90
        if(pGeo->base.tilt > deg2radf(90))
            pGeo->base.tilt -= deg2radf(360);

		// Construct the data that was not transmitted
		structInitDCM(pGeo->gimbalDcm);
		structInitDCM(pGeo->cameraDcm);

		// ECEF position and velocity
        llaToECEFandTrig(&(pGeo->base.posLat), pGeo->posECEF, &pGeo->llaTrig);
        nedToECEFtrigf(pGeo->base.velNED, pGeo->velECEF, &pGeo->llaTrig);

		// Rotation from gimbal to nav
        quaternionToDCM(pGeo->base.gimbalQuat, &pGeo->gimbalDcm);

		// Gimbals Euler attitude
		pGeo->gimbalEuler[AXIS_ROLL]  = dcmRoll(&pGeo->gimbalDcm);
		pGeo->gimbalEuler[AXIS_PITCH] = dcmPitch(&pGeo->gimbalDcm);
		pGeo->gimbalEuler[AXIS_YAW]   = dcmYaw(&pGeo->gimbalDcm);

        // Offset the pan/tilt angles with the current estab output shifts
        Pan  = subtractAnglesf(pGeo->base.pan,  pGeo->base.outputShifts[GIMBAL_AXIS_PAN]);
        Tilt = subtractAnglesf(pGeo->base.tilt, pGeo->base.outputShifts[GIMBAL_AXIS_TILT]);
        
        // Convert tilt from -180 to 180 into -270 to 90
        pGeo->base.tilt = wrapAngle90f(pGeo->base.tilt);

		// Rotation from camera to gimbal, note that this only works if pan
		// is over tilt (pan first, then tilt, just like Euler)
        setDCMBasedOnPanTilt(&tempDcm, Pan, Tilt);

		// Now create the rotation from camera to nav.
        matrixMultiplyf(&pGeo->gimbalDcm, &tempDcm, &pGeo->cameraDcm);

		// The cameras quaternion and Euler angles
		dcmToQuaternion(&pGeo->cameraDcm, pGeo->cameraQuat);
		pGeo->cameraEuler[AXIS_ROLL]  = dcmRoll(&pGeo->cameraDcm);
		pGeo->cameraEuler[AXIS_PITCH] = dcmPitch(&pGeo->cameraDcm);
		pGeo->cameraEuler[AXIS_YAW]   = dcmYaw(&pGeo->cameraDcm);

		// Slant range is the vector magnitude of the line of sight ECEF vector
        pGeo->slantRange = vector3Lengthf(pGeo->base.losECEF);

        // Gimbal ECEF position + line of sight ECEF vector = ECEF image position
        vector3Sum(pGeo->posECEF, vector3Convertf(pGeo->base.losECEF, pGeo->imagePosECEF), pGeo->imagePosECEF);

        // Convert ECEF image position to LLA
        ecefToLLA(pGeo->imagePosECEF, pGeo->imagePosLLA);

		return TRUE;
	}
	else
		return FALSE;

}// DecodeGeolocateTelemetry


/*!
 * Given a current image location compute a new location based on a angular
 * deviation in camera frame (i.e. user click) and assuming the altitude of the
 * new location is the same as the image location altitude.
 * \param geo is the geolocate telemetry from the gimbal.
 * \param imagePosLLA is the latitude, longitude, altitude of the current image
 *        location in radians, radians, meters.
 * \param ydev is the angular deviation in radians from the image location in
 *        right camera direction.
 * \param zdev is the angular deviation in radians from the image location in
 *        up camera direction.
 * \param newPosLLA receives the position of the user click
 */
BOOL offsetImageLocation(const GeolocateTelemetry_t *geo, const double imagePosLLA[NLLA], float ydev, float zdev, double newPosLLA[NLLA])
{
    float range, down;
    float vectorNED[NNED];
    float shift[NECEF];

    // Numerical problems at the poles
    if(geo->llaTrig.cosLat == 0)
        return FALSE;

    // Vector from the gimbal to the image location in NED, notice that Altitue and Down have different signs
    vectorNED[NORTH] = (float)(imagePosLLA[LAT] - geo->base.posLat)*datum_meanRadius;
    vectorNED[EAST]  = (float)(imagePosLLA[LON] - geo->base.posLon)*datum_meanRadius*geo->llaTrig.cosLat;
    vectorNED[DOWN]  = (float)(geo->base.posAlt - imagePosLLA[ALT]);

    // Remember this value
    down = vectorNED[DOWN];

    // Second bail out point, the image altitude must be lower than the gimbal
    // altitude (i.e. down must be positive)
    if(down <= 0)
        return FALSE;

    // Range from gimbal to image position.
    range = vector3Lengthf(vectorNED);

    // Adjust the angular deviations to be deviations in meters
    ydev = tanf(ydev)*range;
    zdev = tanf(zdev)*range;

    // The vector of shifts in *camera* frame. Notice that our
    // definition of Z changed. It was given to us positive up, but
    // native axis for the camera is positive down.
    shift[VECTOR3X] = 0;
    shift[VECTOR3Y] = ydev;
    shift[VECTOR3Z] = -zdev;

    // Rotate this shift from camera frame to NED (body to nav)
    dcmApplyRotation(&geo->cameraDcm, shift, shift);

    // Add this to the vector that goes from gimbal to image,
    // to create a new vector that goes from gimbal to new location.
    vector3Sumf(vectorNED, shift, vectorNED);

    // Last bail out point. The new location altitude must be lower than the
    // gimbal altitude (i.e. down must be positive)
    if(vectorNED[DOWN] <= 0)
        return FALSE;

    // Make the vector longer or shorter until it hits the same altitude as
    // before, i.e. it has the same down component as before
    vector3Scalef(vectorNED, vectorNED, down/vectorNED[DOWN]);

    // Finally compute the new location
    newPosLLA[LAT] = addAngles(geo->base.posLat, vectorNED[NORTH]/datum_meanRadius);
    newPosLLA[LON] = addAngles(geo->base.posLon, vectorNED[EAST]/(datum_meanRadius*geo->llaTrig.cosLat));
    newPosLLA[ALT] = geo->base.posAlt - vectorNED[DOWN];

    return TRUE;

}// offsetImageLocation


/*! Get the terrain intersection of the current line of sight given gimbal geolocate telemetry data.
 *  \param pGeo[in] A pointer to incoming geolocate telemetry data
 *  \param getElevationHAE[in] Pointer to a terrain model lookup function, which should take a lat/lon
 *                             pair (in radians) and return the height above ellipsoid of that point
 *  \param PosLLA[out] Terrain intersection location in the LLA frame
 *  \param pRange[out] Target range in meters to be sent to the gimbal
 *  \return TRUE if a valid intersection was found, otherwise FALSE. Note that if this function
 *          returns FALSE, the data in PosLLA and pRange will still be overwritten with invalid data.
 */
BOOL getTerrainIntersection(const GeolocateTelemetry_t *pGeo, float (*getElevationHAE)(double, double), double PosLLA[NLLA], double *pRange)
{
    double UnitNED[NNED], UnitECEF[NECEF], Step, End;
    float Temp[NNED] = { 1.0f, 0.0f, 0.0f };

    // Coarse and fine line of sight ray step distances, in meters
    static const double StepCoarse = 30.0, StepFine = 1.0;

    // Maximum distance to follow a ray before giving up
    static const double MaxDistance = 5000.0;

    // Rotate a unit line of sight vector by the camera DCM to get a 1-meter NED look vector
    dcmApplyRotation(&pGeo->cameraDcm, Temp, Temp);

    // Convert the unit vector in Temp[NNED] from single to double precision
    vector3Convertf(Temp, UnitNED);

    // Convert the unit vector to ECEF
    nedToECEFtrig(UnitNED, UnitECEF, &pGeo->llaTrig);

    // Start with a step value of StepCoarse and loop until MaxDistance
    Step = StepCoarse;
    End = MaxDistance;

    // Loop through LOS ranges
    for (*pRange = Step; *pRange <= End; *pRange += Step)
    {
        double LineOfSight[NECEF], GroundHeight;

        // Scale the unit LOS vector to the appropriate range
        vector3Scale(UnitECEF, LineOfSight, *pRange);

        // Now add the gimbal position to the line of sight vector to get ECEF position
        vector3Sum(pGeo->posECEF, LineOfSight, LineOfSight);

        // Convert the ECEF line of sight position to LLA
        ecefToLLA(LineOfSight, PosLLA);

        // Get the ground HAE
        GroundHeight = getElevationHAE(PosLLA[LAT], PosLLA[LON]);

        // If the end of this ray is under ground
        if (PosLLA[ALT] <= GroundHeight)
        {
            // If we're using a coarse step
            if (Step == StepCoarse)
            {
                // Back up one step
                *pRange -= Step;

                // Change to the fine step and loop until the current range
                Step = StepFine;
                End = *pRange + StepCoarse;
            }
            // If we're fine stepping, we've found the terrain intersection
            else
            {
                // Clamp the altitude to the ground and tell the caller that the image position is good
                PosLLA[ALT] = GroundHeight;
                return TRUE;
            }
        }
    }

    // No valid image position
    return FALSE;

}// getTerrainIntersection

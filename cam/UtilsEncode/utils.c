#include "utils.h"

// Mandatory vehicle CAM fields. RMC supplies position, speed and heading;
// values absent from this data source must not be fabricated.
CAM_t* create_cam_message(int lat, int lon, int generationDeltaTime, int speed, int heading) {
    CAM_t* cam = (CAM_t*)calloc(1, sizeof(CAM_t));
    if (!cam) return NULL;
    cam->header.protocolVersion = 2;
    cam->header.messageID = 2;
    cam->header.stationID = 4316;
    cam->cam.generationDeltaTime = generationDeltaTime;
    BasicContainer_t* basic = &cam->cam.camParameters.basicContainer;
    basic->stationType = 5;
    basic->referencePosition.latitude = lat;
    basic->referencePosition.longitude = lon;
    basic->referencePosition.positionConfidenceEllipse.semiMajorConfidence = SemiAxisLength_unavailable;
    basic->referencePosition.positionConfidenceEllipse.semiMinorConfidence = SemiAxisLength_unavailable;
    basic->referencePosition.positionConfidenceEllipse.semiMajorOrientation = HeadingValue_unavailable;
    basic->referencePosition.altitude.altitudeValue = AltitudeValue_unavailable;
    basic->referencePosition.altitude.altitudeConfidence = AltitudeConfidence_unavailable;
    cam->cam.camParameters.highFrequencyContainer.present = HighFrequencyContainer_PR_basicVehicleContainerHighFrequency;
    BasicVehicleContainerHighFrequency_t* hf = &cam->cam.camParameters.highFrequencyContainer.choice.basicVehicleContainerHighFrequency;
    hf->heading.headingValue = heading;
    hf->heading.headingConfidence = HeadingConfidence_unavailable;
    hf->speed.speedValue = speed;
    hf->speed.speedConfidence = SpeedConfidence_unavailable;
    hf->driveDirection = DriveDirection_unavailable;
    hf->vehicleLength.vehicleLengthValue = VehicleLengthValue_unavailable;
    hf->vehicleLength.vehicleLengthConfidenceIndication = VehicleLengthConfidenceIndication_unavailable;
    hf->vehicleWidth = VehicleWidth_unavailable;
    hf->longitudinalAcceleration.longitudinalAccelerationValue = LongitudinalAccelerationValue_unavailable;
    hf->longitudinalAcceleration.longitudinalAccelerationConfidence = AccelerationConfidence_unavailable;
    hf->curvature.curvatureValue = CurvatureValue_unavailable;
    hf->curvature.curvatureConfidence = CurvatureConfidence_unavailable;
    hf->curvatureCalculationMode = CurvatureCalculationMode_unavailable;
    hf->yawRate.yawRateValue = YawRateValue_unavailable;
    hf->yawRate.yawRateConfidence = YawRateConfidence_unavailable;
    return cam;
}

ritorno encode_cam_message(const CAM_t* cam) {
    ritorno ritorno;
    asn_encode_to_new_buffer_result_t res;
    
    int encoding = ATS_UNALIGNED_CANONICAL_PER;

    res = asn_encode_to_new_buffer(0, encoding, &asn_DEF_CAM, cam);
    if(res.result.encoded == -1){
        fprintf(stderr, "Could not encode CAM (at %s)\n",
        res.result.failed_type ? res.result.failed_type->name : "unknown");
        ritorno.r = res;
        ritorno.b = false;
    } else {
        //if(res.buffer){
        //    char stringa_uper[res.result.encoded];
        //    for(size_t i=0; i<res.result.encoded; i++){
        //        snprintf(&stringa_uper[i], res.result.encoded, "%02x", (char *)res.buffer);
        //        printf("%02x", ((char *)res.buffer)[i]);
        //    }
        //}
        ritorno.r = res;
        ritorno.b = true;
        }

    return ritorno;  
}

void bin_to_hex(void *bin, size_t bin_len, char **hex_str) {
    *hex_str = (char *)malloc(bin_len * 2 + 1); // Allocate memory for hex string
    if (*hex_str == NULL) {
        return; // Handle allocation failure
    }

    uint8_t *bytes = (uint8_t *)bin; // Cast void* to uint8_t*
    
    for (size_t i = 0; i < bin_len; i++) {
        sprintf(*hex_str + (i * 2), "%02x", bytes[i]); // Convert each byte to 2 hex characters
    }
    
    (*hex_str)[bin_len * 2] = '\0'; // Null-terminate the string
}


void process_hex_string(const char *hex_str) {
    printf("%s", hex_str);
    fflush(stdout);
}

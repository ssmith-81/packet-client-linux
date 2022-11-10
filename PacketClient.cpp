//=============================================================================
// Copyright � 2014 NaturalPoint, Inc. All Rights Reserved.
// 
// This software is provided by the copyright holders and contributors "as is" and
// any express or implied warranties, including, but not limited to, the implied
// warranties of merchantability and fitness for a particular purpose are disclaimed.
// In no event shall NaturalPoint, Inc. or contributors be liable for any direct,
// indirect, incidental, special, exemplary, or consequential damages
// (including, but not limited to, procurement of substitute goods or services;
// loss of use, data, or profits; or business interruption) however caused
// and on any theory of liability, whether in contract, strict liability,
// or tort (including negligence or otherwise) arising in any way out of
// the use of this software, even if advised of the possibility of such damage.
//=============================================================================

/*

PacketClient.cpp

Decodes NatNet packets directly.

Usage [optional]:

	PacketClient [ServerIP] [LocalIP]

	[ServerIP]			IP address of server ( defaults to local machine)
	[LocalIP]			IP address of client ( defaults to local machine)

*/

/*
 * 09.02.2018
 *
 * Adapted for Linux from NatNet SDK 3.0.1.0
 *
 * Boris Belousov (boris@robot-learning.de)
 */

#include <iostream>
#include <cstdio>
#include <cinttypes>
#include <climits>
#include <cstring>
#include <chrono>
#include <thread>

#include <ros/ros.h>
#include <geometry_msgs/PoseStamped.h>

#include <unistd.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <netdb.h>

#define NAT_CONNECT                 0
#define NAT_SERVERINFO              1
#define NAT_REQUEST                 2
#define NAT_RESPONSE                3
#define NAT_REQUEST_MODELDEF        4
#define NAT_MODELDEF                5
#define NAT_REQUEST_FRAMEOFDATA     6
#define NAT_FRAMEOFDATA             7
#define NAT_MESSAGESTRING           8
#define NAT_UNRECOGNIZED_REQUEST    100

#define MAX_PACKETSIZE              100000    // actual packet size is dynamic
#define MAX_NAMELENGTH              256

typedef struct {
  char szName[MAX_NAMELENGTH];            // sending app's name
  uint8_t Version[4];                     // [major.minor.build.revision]
  uint8_t NatNetVersion[4];               // [major.minor.build.revision]
} sSender;

typedef struct sSender_Server {
  sSender Common;
  // host's high resolution clock frequency (ticks per second)
  uint64_t HighResClockFrequency;
  uint16_t DataPort;
  bool IsMulticast;
  uint8_t MulticastGroupAddress[4];
} sSender_Server;

typedef struct {
  uint16_t iMessage;                      // message ID (e.g. NAT_FRAMEOFDATA)
  uint16_t nDataBytes;                    // Num bytes in payload
  union {
    uint8_t cData[MAX_PACKETSIZE];
    char szData[MAX_PACKETSIZE];
    uint32_t lData[MAX_PACKETSIZE / sizeof(uint32_t)];
    float fData[MAX_PACKETSIZE / sizeof(float)];
    sSender Sender;
    sSender_Server SenderServer;
  } Data;                                 // Payload incoming from NatNet Server
} sPacket;

#define MULTICAST_ADDRESS       "239.255.42.99"
#define PORT_COMMAND            1510      // NatNet Command channel
#define PORT_DATA               1511      // NatNet Data channel

// Sockets
int CommandSocket;
int DataSocket;
in_addr ServerAddress;
sockaddr_in HostAddr;

// Global ROS publisher
ros::Publisher pub;
geometry_msgs::PoseStamped pose;

// Versioning
int NatNetVersion[4] = {2, 10, 0, 0};
int ServerVersion[4] = {0, 0, 0, 0};

// Command mode global variables
int gCommandResponse = 0;
int gCommandResponseSize = 0;
unsigned char gCommandResponseString[PATH_MAX];

// ============================== Data mode ================================ //
// Funtion that assigns a time code values to 5 variables passed as arguments
// Requires an integer from the packet as the timecode and timecodeSubframe
bool DecodeTimecode(unsigned int inTimecode,
                    unsigned int inTimecodeSubframe,
                    int *hour,
                    int *minute,
                    int *second,
                    int *frame,
                    int *subframe) {
  bool bValid = true;

  *hour = (inTimecode >> 24) & 255;
  *minute = (inTimecode >> 16) & 255;
  *second = (inTimecode >> 8) & 255;
  *frame = inTimecode & 255;
  *subframe = inTimecodeSubframe;

  return bValid;
}

// Takes timecode and assigns it to a string
bool TimecodeStringify(unsigned int inTimecode,
                       unsigned int inTimecodeSubframe,
                       char *Buffer,
                       size_t BufferSize) {
  bool bValid;
  int hour, minute, second, frame, subframe;
  bValid = DecodeTimecode(inTimecode,
                          inTimecodeSubframe,
                          &hour,
                          &minute,
                          &second,
                          &frame,
                          &subframe);

  snprintf(Buffer, BufferSize, "%2d:%2d:%2d:%2d.%d",
           hour, minute, second, frame, subframe);
  for (unsigned int i = 0; i < strlen(Buffer); i++)
    if (Buffer[i] == ' ')
      Buffer[i] = '0';

  return bValid;
}

void DecodeMarkerID(int sourceID, int *pOutEntityID, int *pOutMemberID) {
  if (pOutEntityID)
    *pOutEntityID = sourceID >> 16;

  if (pOutMemberID)
    *pOutMemberID = sourceID & 0x0000ffff;
}

// *********************************************************************
//
//  Unpack Data:
//      Recieves pointer to bytes that represent a packet of data
//
//      There are lots of print statements that show what
//      data is being stored
//
//      Most memcpy functions will assign the data to a variable.
//      Use this variable at your discretion.
//      Variables created for storing data do not exceed the
//      scope of this function.
//
// *********************************************************************
void Unpack(char *pData) {
  // Checks for NatNet Version number. Used later in function.
  // Packets may be different depending on NatNet version.
  int major = NatNetVersion[0];
  int minor = NatNetVersion[1];

  char *ptr = pData;

  // -----ROS publshing--------


  pose.header.stamp = ros::Time::now();

  pose.header.frame_id="/map";

  
  //----------------------

  printf("Begin Packet\n-------\n");

  // First 2 Bytes is message ID
  int MessageID = 0;
  memcpy(&MessageID, ptr, 2);
  ptr += 2;
  printf("Message ID : %d\n", MessageID);

  // Second 2 Bytes is the size of the packet
  int nBytes = 0;
  memcpy(&nBytes, ptr, 2);
  ptr += 2;
  printf("Byte count : %d\n", nBytes);

  if (MessageID == 7)      // FRAME OF MOCAP DATA packet
  {
    // Next 4 Bytes is the frame number
    int frameNumber = 0;
    memcpy(&frameNumber, ptr, 4);
    ptr += 4;
    printf("Frame # : %d\n", frameNumber);

    // Next 4 Bytes is the number of data sets (markersets, rigidbodies, etc)
    int nMarkerSets = 0;
    memcpy(&nMarkerSets, ptr, 4);
    ptr += 4;
    printf("Marker Set Count : %d\n", nMarkerSets);

    // Loop through number of marker sets and get name and data
    for (int i = 0; i < nMarkerSets; i++) {
      // Markerset name
      char szName[256];
      strcpy(szName, ptr);
      int nDataBytes = (int) strlen(szName) + 1;
      ptr += nDataBytes;
      printf("Model Name: %s\n", szName);

      // marker data
      int nMarkers = 0;
      memcpy(&nMarkers, ptr, 4);
      ptr += 4;
      printf("Marker Count : %d\n", nMarkers);

      for (int j = 0; j < nMarkers; j++) {
        float x = 0;
        memcpy(&x, ptr, 4);
        ptr += 4;
        float y = 0;
        memcpy(&y, ptr, 4);
        ptr += 4;
        float z = 0;
        memcpy(&z, ptr, 4);
        ptr += 4;
        printf("\tMarker %d : [x=%3.2f,y=%3.2f,z=%3.2f]\n", j, x, y, z);
      }
    }

    // Loop through unlabeled markers
    int nOtherMarkers = 0;
    memcpy(&nOtherMarkers, ptr, 4);
    ptr += 4;
    // OtherMarker list is Deprecated
    //printf("Unidentified Marker Count : %d\n", nOtherMarkers);
    for (int j = 0; j < nOtherMarkers; j++) {
      float x = 0.0f;
      memcpy(&x, ptr, 4);
      ptr += 4;
      float y = 0.0f;
      memcpy(&y, ptr, 4);
      ptr += 4;
      float z = 0.0f;
      memcpy(&z, ptr, 4);
      ptr += 4;

      // Deprecated
      //printf("\tMarker %d : pos = [%3.2f,%3.2f,%3.2f]\n",j,x,y,z);
    }

    // Loop through rigidbodies
    int nRigidBodies = 0;
    memcpy(&nRigidBodies, ptr, 4);
    ptr += 4;
    printf("Rigid Body Count : %d\n", nRigidBodies);
    for (int j = 0; j < nRigidBodies; j++) {
      // Rigid body position and orientation
      int ID = 0;
      memcpy(&ID, ptr, 4);
      ptr += 4;
      float x = 0.0f;
      memcpy(&x, ptr, 4);
      ptr += 4;
      float y = 0.0f;
      memcpy(&y, ptr, 4);
      ptr += 4;
      float z = 0.0f;
      memcpy(&z, ptr, 4);
      ptr += 4;
      float qx = 0;
      memcpy(&qx, ptr, 4);
      ptr += 4;
      float qy = 0;
      memcpy(&qy, ptr, 4);
      ptr += 4;
      float qz = 0;
      memcpy(&qz, ptr, 4);
      ptr += 4;
      float qw = 0;
      memcpy(&qw, ptr, 4);
      ptr += 4;
      printf("ID : %d\n", ID);
      printf("pos: [%3.2f,%3.2f,%3.2f]\n", x, y, z);
      printf("ori: [%3.2f,%3.2f,%3.2f,%3.2f]\n", qx, qy, qz, qw);

      // this publishing location will work for only 1 rigid body
      pose.pose.position.x=x;
      pose.pose.position.y=y;
      pose.pose.position.z=z;

      pose.pose.orientation.x = qx;
      pose.pose.orientation.y = qy;
      pose.pose.orientation.z = qz;
      pose.pose.orientation.w = qw;

      pub.publish(pose);
          
          //printf("test",1);
     // printf("test\n--\n");

      // Before NatNet 3.0, marker data was here
      if (major < 3) {
        // associated marker positions
        int nRigidMarkers = 0;
        memcpy(&nRigidMarkers, ptr, 4);
        ptr += 4;
        printf("Marker Count: %d\n", nRigidMarkers);
        int nBytes = nRigidMarkers * 3 * sizeof(float);
        float *markerData = (float *) malloc(nBytes);
        memcpy(markerData, ptr, nBytes);
        ptr += nBytes;

        if (major >= 2) {
          // associated marker IDs
          nBytes = nRigidMarkers * sizeof(int);
          int *markerIDs = (int *) malloc(nBytes);
          memcpy(markerIDs, ptr, nBytes);
          ptr += nBytes;

          // associated marker sizes
          nBytes = nRigidMarkers * sizeof(float);
          float *markerSizes = (float *) malloc(nBytes);
          memcpy(markerSizes, ptr, nBytes);
          ptr += nBytes;

          for (int k = 0; k < nRigidMarkers; k++) {
            printf("\tMarker %d: id=%d\tsize=%3.1f\tpos=[%3.2f,%3.2f,%3.2f]\n",
                   k,
                   markerIDs[k],
                   markerSizes[k],
                   markerData[k * 3],
                   markerData[k * 3 + 1],
                   markerData[k * 3 + 2]);
          }

          if (markerIDs)
            free(markerIDs);
          if (markerSizes)
            free(markerSizes);

        } else {
          for (int k = 0; k < nRigidMarkers; k++) {
            printf("\tMarker %d: pos = [%3.2f,%3.2f,%3.2f]\n", k,
                   markerData[k * 3], markerData[k * 3 + 1],
                   markerData[k * 3 + 2]);
          }
        }
        if (markerData)
          free(markerData);
      }

      // NatNet version 2.0 and later
      if (major >= 2) {
        // Mean marker error
        float fError = 0.0f;
        memcpy(&fError, ptr, 4);
        ptr += 4;
        printf("Mean marker error: %3.2f\n", fError);
      }

      // NatNet version 2.6 and later
      if (((major == 2) && (minor >= 6)) || (major > 2)) {
        // params
        short params = 0;
        memcpy(&params, ptr, 2);
        ptr += 2;
        // 0x01 : rigid body was successfully tracked in this frame
        bool bTrackingValid = params & 0x01;
        if (bTrackingValid) {
          printf("Tracking Valid: True\n");
        } else {
          printf("Tracking Valid: False\n");
        }
      }

    } // Go to next rigid body


    // Skeletons (NatNet version 2.1 and later)
    if (((major == 2) && (minor > 0)) || (major > 2)) {
      int nSkeletons = 0;
      memcpy(&nSkeletons, ptr, 4);
      ptr += 4;
      printf("Skeleton Count : %d\n", nSkeletons);

      // Loop through skeletons
      for (int j = 0; j < nSkeletons; j++) {
        // skeleton id
        int skeletonID = 0;
        memcpy(&skeletonID, ptr, 4);
        ptr += 4;

        // Number of rigid bodies (bones) in skeleton
        int nRigidBodies = 0;
        memcpy(&nRigidBodies, ptr, 4);
        ptr += 4;
        printf("Rigid Body Count : %d\n", nRigidBodies);

        // Loop through rigid bodies (bones) in skeleton
        for (int j = 0; j < nRigidBodies; j++) {
          // Rigid body position and orientation
          int ID = 0;
          memcpy(&ID, ptr, 4);
          ptr += 4;
          float x = 0.0f;
          memcpy(&x, ptr, 4);
          ptr += 4;
          float y = 0.0f;
          memcpy(&y, ptr, 4);
          ptr += 4;
          float z = 0.0f;
          memcpy(&z, ptr, 4);
          ptr += 4;
          float qx = 0;
          memcpy(&qx, ptr, 4);
          ptr += 4;
          float qy = 0;
          memcpy(&qy, ptr, 4);
          ptr += 4;
          float qz = 0;
          memcpy(&qz, ptr, 4);
          ptr += 4;
          float qw = 0;
          memcpy(&qw, ptr, 4);
          ptr += 4;
          printf("ID : %d\n", ID);
          printf("pos: [%3.2f,%3.2f,%3.2f]\n", x, y, z);
          printf("ori: [%3.2f,%3.2f,%3.2f,%3.2f]\n", qx, qy, qz, qw);

          

          // Before NatNet 3.0, marker data was here
          if (major < 3) {
            // associated marker positions
            int nRigidMarkers = 0;
            memcpy(&nRigidMarkers, ptr, 4);
            ptr += 4;
            printf("Marker Count: %d\n", nRigidMarkers);
            int nBytes = nRigidMarkers * 3 * sizeof(float);
            float *markerData = (float *) malloc(nBytes);
            memcpy(markerData, ptr, nBytes);
            ptr += nBytes;

            if (major >= 2) {
              // associated marker IDs
              nBytes = nRigidMarkers * sizeof(int);
              int *markerIDs = (int *) malloc(nBytes);
              memcpy(markerIDs, ptr, nBytes);
              ptr += nBytes;

              // associated marker sizes
              nBytes = nRigidMarkers * sizeof(float);
              float *markerSizes = (float *) malloc(nBytes);
              memcpy(markerSizes, ptr, nBytes);
              ptr += nBytes;

              for (int k = 0; k < nRigidMarkers; k++) {
                printf("\tMarker %d: id=%d\tsize=%3.1f\tpos=[%3.2f,%3.2f,%3.2f]\n",
                       k,
                       markerIDs[k],
                       markerSizes[k],
                       markerData[k * 3],
                       markerData[k * 3 + 1],
                       markerData[k * 3 + 2]);
              }

              if (markerIDs)
                free(markerIDs);
              if (markerSizes)
                free(markerSizes);

            } else {
              for (int k = 0; k < nRigidMarkers; k++) {
                printf("\tMarker %d: pos = [%3.2f,%3.2f,%3.2f]\n", k,
                       markerData[k * 3], markerData[k * 3 + 1],
                       markerData[k * 3 + 2]);
              }
            }
            if (markerData)
              free(markerData);
          }

          // Mean marker error (NatNet version 2.0 and later)
          if (major >= 2) {
            float fError = 0.0f;
            memcpy(&fError, ptr, 4);
            ptr += 4;
            printf("Mean marker error: %3.2f\n", fError);
          }

          // Tracking flags (NatNet version 2.6 and later)
          if (((major == 2) && (minor >= 6)) || (major > 2)) {
            // params
            short params = 0;
            memcpy(&params, ptr, 2);
            ptr += 2;
            // 0x01 : rigid body was successfully tracked in this frame
            bool bTrackingValid = params & 0x01;
          }

        } // next rigid body

      } // next skeleton
    }

    // labeled markers (NatNet version 2.3 and later)
    if (((major == 2) && (minor >= 3)) || (major > 2)) {
      int nLabeledMarkers = 0;
      memcpy(&nLabeledMarkers, ptr, 4);
      ptr += 4;
      printf("Labeled Marker Count : %d\n", nLabeledMarkers);

      // Loop through labeled markers
      for (int j = 0; j < nLabeledMarkers; j++) {
        // id
        // Marker ID Scheme:
        // Active Markers:
        //   ID = ActiveID, correlates to RB ActiveLabels list
        // Passive Markers:
        //   If Asset with Legacy Labels
        //      AssetID 	(Hi Word)
        //      MemberID	(Lo Word)
        //   Else
        //      PointCloud ID
        int ID = 0;
        memcpy(&ID, ptr, 4);
        ptr += 4;
        int modelID, markerID;
        DecodeMarkerID(ID, &modelID, &markerID);


        // x
        float x = 0.0f;
        memcpy(&x, ptr, 4);
        ptr += 4;
        // y
        float y = 0.0f;
        memcpy(&y, ptr, 4);
        ptr += 4;
        // z
        float z = 0.0f;
        memcpy(&z, ptr, 4);
        ptr += 4;
        // size
        float size = 0.0f;
        memcpy(&size, ptr, 4);
        ptr += 4;

        // NatNet version 2.6 and later
        if (((major == 2) && (minor >= 6)) || (major > 2)) {
          // marker params
          short params = 0;
          memcpy(&params, ptr, 2);
          ptr += 2;
          // marker was not visible (occluded) in this frame
          bool bOccluded = (params & 0x01) != 0;
          // position provided by point cloud solve
          bool bPCSolved = (params & 0x02) != 0;
          // position provided by model solve
          bool bModelSolved = (params & 0x04) != 0;
          if (major >= 3) {
            // marker has an associated model
            bool bHasModel = (params & 0x08) != 0;
            // marker is an unlabeled marker
            bool bUnlabeled = (params & 0x10) != 0;
            // marker is an active marker
            bool bActiveMarker = (params & 0x20) != 0;
          }

        }

        // NatNet version 3.0 and later
        float residual = 0.0f;
        if (major >= 3) {
          // Marker residual
          memcpy(&residual, ptr, 4);
          ptr += 4;
        }

        printf("ID  : [MarkerID: %d] [ModelID: %d]\n", markerID, modelID);
        printf("pos : [%3.2f,%3.2f,%3.2f]\n", x, y, z);
        printf("size: [%3.2f]\n", size);
        printf("err:  [%3.2f]\n", residual);
      }
    }

    // Force Plate data (NatNet version 2.9 and later)
    if (((major == 2) && (minor >= 9)) || (major > 2)) {
      int nForcePlates;
      memcpy(&nForcePlates, ptr, 4);
      ptr += 4;
      for (int iForcePlate = 0; iForcePlate < nForcePlates; iForcePlate++) {
        // ID
        int ID = 0;
        memcpy(&ID, ptr, 4);
        ptr += 4;
        printf("Force Plate : %d\n", ID);

        // Channel Count
        int nChannels = 0;
        memcpy(&nChannels, ptr, 4);
        ptr += 4;

        // Channel Data
        for (int i = 0; i < nChannels; i++) {
          printf(" Channel %d : ", i);
          int nFrames = 0;
          memcpy(&nFrames, ptr, 4);
          ptr += 4;
          for (int j = 0; j < nFrames; j++) {
            float val = 0.0f;
            memcpy(&val, ptr, 4);
            ptr += 4;
            printf("%3.2f   ", val);
          }
          printf("\n");
        }
      }
    }

    // Device data (NatNet version 3.0 and later)
    if (((major == 2) && (minor >= 11)) || (major > 2)) {
      int nDevices;
      memcpy(&nDevices, ptr, 4);
      ptr += 4;
      for (int iDevice = 0; iDevice < nDevices; iDevice++) {
        // ID
        int ID = 0;
        memcpy(&ID, ptr, 4);
        ptr += 4;
        printf("Device : %d\n", ID);

        // Channel Count
        int nChannels = 0;
        memcpy(&nChannels, ptr, 4);
        ptr += 4;

        // Channel Data
        for (int i = 0; i < nChannels; i++) {
          printf(" Channel %d : ", i);
          int nFrames = 0;
          memcpy(&nFrames, ptr, 4);
          ptr += 4;
          for (int j = 0; j < nFrames; j++) {
            float val = 0.0f;
            memcpy(&val, ptr, 4);
            ptr += 4;
            printf("%3.2f   ", val);
          }
          printf("\n");
        }
      }
    }

    // software latency (removed in version 3.0)
    if (major < 3) {
      float softwareLatency = 0.0f;
      memcpy(&softwareLatency, ptr, 4);
      ptr += 4;
      printf("software latency : %3.3f\n", softwareLatency);
    }

    // timecode
    unsigned int timecode = 0;
    memcpy(&timecode, ptr, 4);
    ptr += 4;
    unsigned int timecodeSub = 0;
    memcpy(&timecodeSub, ptr, 4);
    ptr += 4;
    char szTimecode[128] = "";
    TimecodeStringify(timecode, timecodeSub, szTimecode, 128);

    // timestamp
    double timestamp = 0.0f;

    // NatNet version 2.7 and later - increased from single to double precision
    if (((major == 2) && (minor >= 7)) || (major > 2)) {
      memcpy(&timestamp, ptr, 8);
      ptr += 8;
    } else {
      float fTemp = 0.0f;
      memcpy(&fTemp, ptr, 4);
      ptr += 4;
      timestamp = (double) fTemp;
    }
    printf("Timestamp : %3.3f\n", timestamp);

    // high res timestamps (version 3.0 and later)
    if (major >= 3) {
      uint64_t cameraMidExposureTimestamp = 0;
      memcpy(&cameraMidExposureTimestamp, ptr, 8);
      ptr += 8;
      printf("Mid-exposure timestamp : %" PRIu64 "\n",
             cameraMidExposureTimestamp);

      uint64_t cameraDataReceivedTimestamp = 0;
      memcpy(&cameraDataReceivedTimestamp, ptr, 8);
      ptr += 8;
      printf("Camera data received timestamp : %" PRIu64 "\n",
             cameraDataReceivedTimestamp);

      uint64_t transmitTimestamp = 0;
      memcpy(&transmitTimestamp, ptr, 8);
      ptr += 8;
      printf("Transmit timestamp : %" PRIu64 "\n", transmitTimestamp);
    }

    // frame params
    short params = 0;
    memcpy(&params, ptr, 2);
    ptr += 2;
    // 0x01 Motive is recording
    bool bIsRecording = (params & 0x01) != 0;
    // 0x02 Actively tracked model list has changed
    bool bTrackedModelsChanged = (params & 0x02) != 0;


    // end of data tag
    int eod = 0;
    memcpy(&eod, ptr, 4);
    ptr += 4;
    printf("End Packet\n-------------\n");

  } else if (MessageID == 5) // Data Descriptions
  {
    // number of datasets
    int nDatasets = 0;
    memcpy(&nDatasets, ptr, 4);
    ptr += 4;
    printf("Dataset Count : %d\n", nDatasets);

    for (int i = 0; i < nDatasets; i++) {
      printf("Dataset %d\n", i);

      int type = 0;
      memcpy(&type, ptr, 4);
      ptr += 4;
      printf("Type : %d\n", type);

      if (type == 0)   // markerset
      {
        // name
        char szName[256];
        strcpy(szName, ptr);
        int nDataBytes = (int) strlen(szName) + 1;
        ptr += nDataBytes;
        printf("Markerset Name: %s\n", szName);

        // marker data
        int nMarkers = 0;
        memcpy(&nMarkers, ptr, 4);
        ptr += 4;
        printf("Marker Count : %d\n", nMarkers);

        for (int j = 0; j < nMarkers; j++) {
          char szName[256];
          strcpy(szName, ptr);
          int nDataBytes = (int) strlen(szName) + 1;
          ptr += nDataBytes;
          printf("Marker Name: %s\n", szName);
        }
      } else if (type == 1)   // rigid body
      {
        if (major >= 2) {
          // name
          char szName[MAX_NAMELENGTH];
          strcpy(szName, ptr);
          ptr += strlen(ptr) + 1;
          printf("Name: %s\n", szName);
        }

        int ID = 0;
        memcpy(&ID, ptr, 4);
        ptr += 4;
        printf("ID : %d\n", ID);

        int parentID = 0;
        memcpy(&parentID, ptr, 4);
        ptr += 4;
        printf("Parent ID : %d\n", parentID);

        float xoffset = 0;
        memcpy(&xoffset, ptr, 4);
        ptr += 4;
        printf("X Offset : %3.2f\n", xoffset);

        float yoffset = 0;
        memcpy(&yoffset, ptr, 4);
        ptr += 4;
        printf("Y Offset : %3.2f\n", yoffset);

        float zoffset = 0;
        memcpy(&zoffset, ptr, 4);
        ptr += 4;
        printf("Z Offset : %3.2f\n", zoffset);

        // Per-marker data (NatNet 3.0 and later)
        if (major >= 3) {
          int nMarkers = 0;
          memcpy(&nMarkers, ptr, 4);
          ptr += 4;

          // Marker positions
          nBytes = nMarkers * 3 * sizeof(float);
          float *markerPositions = (float *) malloc(nBytes);
          memcpy(markerPositions, ptr, nBytes);
          ptr += nBytes;

          // Marker required active labels
          nBytes = nMarkers * sizeof(int);
          int *markerRequiredLabels = (int *) malloc(nBytes);
          memcpy(markerRequiredLabels, ptr, nBytes);
          ptr += nBytes;

          for (int markerIdx = 0; markerIdx < nMarkers; ++markerIdx) {
            float *markerPosition = markerPositions + markerIdx * 3;
            const int markerRequiredLabel = markerRequiredLabels[markerIdx];

            printf("\tMarker #%d:\n", markerIdx);
            printf("\t\tPosition: %.2f, %.2f, %.2f\n",
                   markerPosition[0],
                   markerPosition[1],
                   markerPosition[2]);

            if (markerRequiredLabel != 0) {
              printf("\t\tRequired active label: %d\n", markerRequiredLabel);
            }
          }

          free(markerPositions);
          free(markerRequiredLabels);
        }
      } else if (type == 2)   // skeleton
      {
        char szName[MAX_NAMELENGTH];
        strcpy(szName, ptr);
        ptr += strlen(ptr) + 1;
        printf("Name: %s\n", szName);

        int ID = 0;
        memcpy(&ID, ptr, 4);
        ptr += 4;
        printf("ID : %d\n", ID);

        int nRigidBodies = 0;
        memcpy(&nRigidBodies, ptr, 4);
        ptr += 4;
        printf("RigidBody (Bone) Count : %d\n", nRigidBodies);

        for (int i = 0; i < nRigidBodies; i++) {
          if (major >= 2) {
            // RB name
            char szName[MAX_NAMELENGTH];
            strcpy(szName, ptr);
            ptr += strlen(ptr) + 1;
            printf("Rigid Body Name: %s\n", szName);
          }

          int ID = 0;
          memcpy(&ID, ptr, 4);
          ptr += 4;
          printf("RigidBody ID : %d\n", ID);

          int parentID = 0;
          memcpy(&parentID, ptr, 4);
          ptr += 4;
          printf("Parent ID : %d\n", parentID);

          float xoffset = 0;
          memcpy(&xoffset, ptr, 4);
          ptr += 4;
          printf("X Offset : %3.2f\n", xoffset);

          float yoffset = 0;
          memcpy(&yoffset, ptr, 4);
          ptr += 4;
          printf("Y Offset : %3.2f\n", yoffset);

          float zoffset = 0;
          memcpy(&zoffset, ptr, 4);
          ptr += 4;
          printf("Z Offset : %3.2f\n", zoffset);
        }
      }

    }   // next dataset

    printf("End Packet\n-------------\n");

  } else {
    printf("Unrecognized Packet Type.\n");
  }

}

// Data listener thread. Listens for incoming bytes from NatNet
static void *DataListenThread(void *dummy) {
  char szData[20000];
  socklen_t addr_len = sizeof(struct sockaddr);
  sockaddr_in TheirAddress{};

  while (true) {
    // Block until we receive a datagram from the network
    // (from anyone including ourselves)
    ssize_t nDataBytesReceived = recvfrom(DataSocket,
                                          szData,
                                          sizeof(szData),
                                          0,
                                          (sockaddr *) &TheirAddress,
                                          &addr_len);
    // Once we have bytes recieved Unpack organizes all the data
    Unpack(szData);
  }

  return 0;
}

// ============================= Command mode ============================== //
// Send a command to Motive.
int SendCommand(char *szCommand) {
  // reset global result
  gCommandResponse = -1;

  // format command packet
  sPacket commandPacket{};
  strcpy(commandPacket.Data.szData, szCommand);
  commandPacket.iMessage = NAT_REQUEST;
  commandPacket.nDataBytes =
      (unsigned short) (strlen(commandPacket.Data.szData) + 1);

  // send command and wait (a bit)
  // for command response to set global response var in CommandListenThread
  ssize_t iRet = sendto(CommandSocket,
                        (char *) &commandPacket,
                        4 + commandPacket.nDataBytes,
                        0,
                        (sockaddr *) &HostAddr,
                        sizeof(HostAddr));
  if (iRet == -1) {
    printf("Socket error sending command");
  } else {
    int waitTries = 5;
    while (waitTries--) {
      if (gCommandResponse != -1)
        break;
      std::this_thread::sleep_for(std::chrono::milliseconds(30));
    }

    if (gCommandResponse == -1) {
      printf("Command response not received (timeout)");
    } else if (gCommandResponse == 0) {
      printf("Command response received with success");
    } else if (gCommandResponse > 0) {
      printf("Command response received with errors");
    }
  }

  return gCommandResponse;
}

int CreateCommandSocket(in_addr_t IP_Address, unsigned short uPort) {
  struct sockaddr_in my_addr{};
  static unsigned long ivalue;
  static unsigned long bFlag;
  int nlengthofsztemp = 64;
  int sockfd;

  // Create a blocking, datagram socket
  if ((sockfd = socket(AF_INET, SOCK_DGRAM, 0)) == -1) {
    return -1;
  }

  // bind socket
  memset(&my_addr, 0, sizeof(my_addr));
  my_addr.sin_family = AF_INET;
  my_addr.sin_port = htons(uPort);
  my_addr.sin_addr.s_addr = IP_Address;
  if (bind(sockfd,
           (struct sockaddr *) &my_addr,
           sizeof(struct sockaddr)) == -1) {
    close(sockfd);
    return -1;
  }

  // set to broadcast mode
  ivalue = 1;
  if (setsockopt(sockfd,
                 SOL_SOCKET,
                 SO_BROADCAST,
                 (char *) &ivalue,
                 sizeof(ivalue)) == -1) {
    close(sockfd);
    return -1;
  }

  return sockfd;
}

// Command response listener thread
static void *CommandListenThread(void *dummy) {
  char ip_as_str[INET_ADDRSTRLEN];
  ssize_t nDataBytesReceived;
  sockaddr_in TheirAddress{};
  sPacket PacketIn{};
  socklen_t addr_len = sizeof(struct sockaddr);

  while (true) {
    // blocking
    nDataBytesReceived = recvfrom(CommandSocket,
                                  (char *) &PacketIn,
                                  sizeof(sPacket),
                                  0,
                                  (struct sockaddr *) &TheirAddress,
                                  &addr_len);

    if ((nDataBytesReceived == 0) || (nDataBytesReceived == -1))
      continue;

    // debug - print message
    inet_ntop(AF_INET, &(TheirAddress.sin_addr), ip_as_str, INET_ADDRSTRLEN);
    printf("[Client] Received command from %s: Command=%d, nDataBytes=%d\n",
           ip_as_str, (int) PacketIn.iMessage, (int) PacketIn.nDataBytes);

    unsigned char *ptr = (unsigned char *) &PacketIn;
    sSender_Server *server_info = (sSender_Server *) (ptr + 4);

    // handle command
    switch (PacketIn.iMessage) {
      case NAT_MODELDEF:std::cout << "[Client] Received NAT_MODELDEF packet";
        Unpack((char *) &PacketIn);
        break;
      case NAT_FRAMEOFDATA:
        std::cout << "[Client] Received NAT_FRAMEOFDATA packet";
        Unpack((char *) &PacketIn);
        break;
      case NAT_SERVERINFO:
        // Streaming app's name, e.g., Motive
        std::cout << server_info->Common.szName << " ";
        // Streaming app's version, e.g., 2.0.0.0
        for (int i = 0; i < 4; ++i) {
          std::cout << static_cast<int>(server_info->Common.Version[i]) << ".";
        }
        std::cout << '\b' << std::endl;
        // Streaming app's NatNet version, e.g., 3.0.0.0
        std::cout << "NatNet ";
        int digit;
        for (int i = 0; i < 4; ++i) {
          digit = static_cast<int>(server_info->Common.NatNetVersion[i]);
          std::cout << digit << ".";
        }
        std::cout << '\b' << std::endl;
        // Save versions in global variables
        for (int i = 0; i < 4; i++) {
          NatNetVersion[i] = server_info->Common.NatNetVersion[i];
          ServerVersion[i] = server_info->Common.Version[i];
        }
        break;
      case NAT_RESPONSE:gCommandResponseSize = PacketIn.nDataBytes;
        if (gCommandResponseSize == 4)
          memcpy(&gCommandResponse,
                 &PacketIn.Data.lData[0],
                 gCommandResponseSize);
        else {
          memcpy(&gCommandResponseString[0],
                 &PacketIn.Data.cData[0],
                 gCommandResponseSize);
          printf("Response : %s", gCommandResponseString);
          gCommandResponse = 0;   // ok
        }
        break;
      case NAT_UNRECOGNIZED_REQUEST:
        printf("[Client] received 'unrecognized request'\n");
        gCommandResponseSize = 0;
        gCommandResponse = 1;       // err
        break;
      case NAT_MESSAGESTRING:
        printf("[Client] Received message: %s\n",
               PacketIn.Data.szData);
        break;
    }
  }

  return 0;
}

// ================================ Main =================================== //
// Convert IP address string to address
bool IPAddress_StringToAddr(char *szNameOrAddress,
                            struct in_addr *Address) {
  int retVal;
  struct sockaddr_in saGNI;
  char hostName[256];
  char servInfo[256];
  u_short port;
  port = 0;

  // Set up sockaddr_in structure which is passed to the getnameinfo function
  saGNI.sin_family = AF_INET;
  saGNI.sin_addr.s_addr = inet_addr(szNameOrAddress);
  saGNI.sin_port = htons(port);

  // getnameinfo in WS2tcpip is protocol independent
  // and resolves address to ANSI host name
  if ((retVal = getnameinfo((sockaddr *) &saGNI, sizeof(sockaddr), hostName,
                            256, servInfo, 256, NI_NUMERICSERV)) != 0) {
    // Returns error if getnameinfo failed
    printf("[PacketClient] GetHostByAddr failed\n");
    return false;
  }

  Address->s_addr = saGNI.sin_addr.s_addr;
  return true;
}

int main(int argc, char *argv[]) {
  int retval;
  char szMyIPAddress[128] = "";
  char szServerIPAddress[128] = "";
  in_addr MyAddress{}, MultiCastAddress{};
  int optval = 0x100000;
  socklen_t optval_size = 4;

  //-----------------------------
  // ROS Section
  int rate_b = 10; //10 hz

  ros::init(argc, argv, "Mocap");

  ros::NodeHandle nh;

  pub = nh.advertise<geometry_msgs::PoseStamped>("/dronePose",1000);//,1,true);


  //----------------------------


  // ================ Read IP addresses
  // server address
  if (argc > 1) {
    strcpy(szServerIPAddress, argv[1]);    // specified on command line
    retval = (int) IPAddress_StringToAddr(szServerIPAddress, &ServerAddress);
  } else {
    printf("Usage:\n\n\tPacketClient [ServerIP] [LocalIP]\n");
  }

  // client address
  if (argc > 2) {
    strcpy(szMyIPAddress, argv[2]);    // specified on command line
    retval = (int) IPAddress_StringToAddr(szMyIPAddress, &MyAddress);
  } else {
    printf("Usage:\n\n\tPacketClient [ServerIP] [LocalIP]\n");
  }
  MultiCastAddress.s_addr = inet_addr(MULTICAST_ADDRESS);
  printf("Client: %s\n", szMyIPAddress);
  printf("Server: %s\n", szServerIPAddress);
  printf("Multicast Group: %s\n", MULTICAST_ADDRESS);


  // ================ Create "Command" socket
  unsigned short port = 0;
  CommandSocket = CreateCommandSocket(MyAddress.s_addr, port);
  if (CommandSocket == -1) {
    // error
    printf("Command socket creation error\n");
  } else {
    // [optional] set to non-blocking
    //u_long iMode=1;
    //ioctlsocket(CommandSocket,FIONBIO,&iMode);
    // set buffer
    setsockopt(CommandSocket, SOL_SOCKET, SO_RCVBUF, (char *) &optval, 4);
    getsockopt(CommandSocket,
               SOL_SOCKET,
               SO_RCVBUF,
               (char *) &optval,
               &optval_size);
    if (optval != 0x100000) {
      // err - actual size...
      printf("[CommandSocket] ReceiveBuffer size = %d\n", optval);
    }
    // startup our "Command Listener" thread
    pthread_t cmd_listen_thread;
    pthread_attr_t cmd_thread_attr{};
    if ((bool) pthread_attr_init(&cmd_thread_attr))
      printf("attributes not set to default\n");
    pthread_create(&cmd_listen_thread,
                   &cmd_thread_attr,
                   CommandListenThread,
                   nullptr);
  }


  // ================ Create "Data" socket
  DataSocket = socket(AF_INET, SOCK_DGRAM, 0);

  // allow multiple clients on same machine to use address/port
  int value = 1;
  retval = setsockopt(DataSocket,
                      SOL_SOCKET,
                      SO_REUSEADDR,
                      (char *) &value,
                      sizeof(value));
  if (retval == -1) {
    close(DataSocket);
    printf("Error while setting DataSocket options\n");
    return -1;
  }

  struct sockaddr_in MySocketAddr{};
  memset(&MySocketAddr, 0, sizeof(MySocketAddr));
  MySocketAddr.sin_family = AF_INET;
  MySocketAddr.sin_port = htons(PORT_DATA);
//  MySocketAddr.sin_addr = MyAddress;
  MySocketAddr.sin_addr.s_addr = htonl(INADDR_ANY);
  if (bind(DataSocket,
           (struct sockaddr *) &MySocketAddr,
           sizeof(struct sockaddr)) == -1) {
    printf("[PacketClient] bind failed\n");
    return 0;
  }
  // join multicast group
  struct ip_mreq Mreq{};
  Mreq.imr_multiaddr = MultiCastAddress;
  Mreq.imr_interface = MyAddress;
  retval = setsockopt(DataSocket,
                      IPPROTO_IP,
                      IP_ADD_MEMBERSHIP,
                      (char *) &Mreq,
                      sizeof(Mreq));
  if (retval == -1) {
    printf("[PacketClient] join failed\n");
    return -1;
  }
  // create a 1MB buffer
  setsockopt(DataSocket, SOL_SOCKET, SO_RCVBUF, (char *) &optval, 4);
  getsockopt(DataSocket, SOL_SOCKET, SO_RCVBUF, (char *) &optval, &optval_size);
  if (optval != 0x100000) {
    printf("[PacketClient] ReceiveBuffer size = %d\n", optval);
  }
  // startup our "Data Listener" thread
  pthread_t data_listen_thread;
  pthread_attr_t data_thread_attr{};
  if ((bool) pthread_attr_init(&data_thread_attr))
    printf("attributes not set to default\n");
  pthread_create(&data_listen_thread,
                 &data_thread_attr,
                 DataListenThread,
                 nullptr);


  // ================ Server address for commands
  memset(&HostAddr, 0, sizeof(HostAddr));
  HostAddr.sin_family = AF_INET;
  HostAddr.sin_port = htons(PORT_COMMAND);
  HostAddr.sin_addr = ServerAddress;

  // send initial connect request
  sPacket PacketOut{};
  PacketOut.iMessage = NAT_CONNECT;
  PacketOut.nDataBytes = 0;
  int nTries = 3;
  while (nTries--) {
    ssize_t iRet = sendto(CommandSocket,
                          (char *) &PacketOut,
                          4 + PacketOut.nDataBytes,
                          0,
                          (sockaddr *) &HostAddr,
                          sizeof(HostAddr));
    if (iRet != -1)
      break;
    printf("Initial connect request failed\n");
  }


  // ================ Main menu
  printf("Packet Client started\n\n");
  printf(
      "Commands:"
          "\ns\tsend data descriptions"
          "\nf\tsend frame of data"
          "\nt\tsend test request"
          "\nq\tquit\n\n");
  int c;
  char szRequest[512];
  bool bExit = false;
  while (!bExit) {
    c = getchar();
    switch (c) {
      case 's':
        // send NAT_REQUEST_MODELDEF command to server
        // (will respond on the "Command Listener" thread)
        PacketOut.iMessage = NAT_REQUEST_MODELDEF;
        PacketOut.nDataBytes = 0;
        nTries = 3;
        while (nTries--) {
          ssize_t iRet = sendto(CommandSocket,
                                (char *) &PacketOut,
                                4 + PacketOut.nDataBytes,
                                0,
                                (sockaddr *) &HostAddr,
                                sizeof(HostAddr));
          if (iRet != -1)
            break;
          printf("REQUEST_MODELDEF failed\n");
        }
        break;
      case 'f':
        // send NAT_REQUEST_FRAMEOFDATA
        // (will respond on the "Command Listener" thread)
        PacketOut.iMessage = NAT_REQUEST_FRAMEOFDATA;
        PacketOut.nDataBytes = 0;
        nTries = 3;
        while (nTries--) {
          ssize_t iRet = sendto(CommandSocket,
                                (char *) &PacketOut,
                                4 + PacketOut.nDataBytes,
                                0,
                                (sockaddr *) &HostAddr,
                                sizeof(HostAddr));
          if (iRet != -1)
            break;
          printf("REQUEST_FRAMEOFDATA failed\n");
        }
        break;
      case 't':
        // send NAT_MESSAGESTRING
        // (will respond on the "Command Listener" thread)
        strcpy(szRequest, "TestRequest");
        PacketOut.iMessage = NAT_REQUEST;
        PacketOut.nDataBytes = (unsigned short) (strlen(szRequest) + 1);
        strcpy(PacketOut.Data.szData, szRequest);
        nTries = 3;
        while (nTries--) {
          ssize_t iRet = sendto(CommandSocket,
                                (char *) &PacketOut,
                                4 + PacketOut.nDataBytes,
                                0,
                                (sockaddr *) &HostAddr,
                                sizeof(HostAddr));
          if (iRet != -1)
            break;
        }
        break;
      case 'w': {
        char szCommand[512];
        int testVal;
        int returnCode;

        testVal = -50;
        sprintf(szCommand, "SetPlaybackStartFrame,%d", testVal);
        returnCode = SendCommand(szCommand);

        testVal = 1500;
        sprintf(szCommand, "SetPlaybackStopFrame,%d", testVal);
        returnCode = SendCommand(szCommand);

        testVal = 0;
        sprintf(szCommand, "SetPlaybackLooping,%d", testVal);
        returnCode = SendCommand(szCommand);

        testVal = 100;
        sprintf(szCommand, "SetPlaybackCurrentFrame,%d", testVal);
        returnCode = SendCommand(szCommand);

      }
        break;
      case 'q':bExit = true;
        break;
      default:break;
    }
  }

  return 0;
}

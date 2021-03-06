/**
 * @file WalkingEngine.cpp
 * Implementation of a module that creates the walking motions
 * @author Colin Graf
 */

#include "WalkingEngine.h"
#include "Tools/Motion/SensorData.h"
#include "Tools/Settings.h"
#include "Tools/Debugging/Annotation.h"
#include "Tools/Debugging/DebugDrawings.h"
#include "Tools/Debugging/DebugDrawings3D.h"
#include "Tools/MessageQueue/InMessage.h"
#include "Tools/Math/Rotation.h"
#include "Tools/Motion/InverseKinematic.h"
#include "Tools/Streams/InStreams.h"
#include <algorithm>
#include <iostream>

using namespace std;
using namespace Joints;
using namespace Sensors;

// small functions
float crop(float x, float min, float max)
{
  if(x < min)
    return min;
  if(x > max)
    return max;
  return x;
}

// x = forward, y = left, z = turn
float evaluateWalkVolume(float x, float y, float z)
{
   // his affects the relationship between forward and left.
   float e = 1.05f; //1.25

   // lower value allows turn to be higher with a high forward/left, higher values don't allow a high turn
   float n = 1;

   float r = 2.0f / e;
   float t = 2.0f / n;

   return pow(pow(x, r) + pow(y, r), (t / r)) + pow(z, t);
}

void WalkingEngine::ellipsoidClampWalk(float &forward, float &left, float &turn, float speed)
{
   const float MIN_SPEED = 0.0f;
   const float MAX_SPEED = 1.0f;
   speed = crop(speed, MIN_SPEED, MAX_SPEED);

   // limit max to 66-100% depending on speed which lies in [0, 1]
   float M_FORWARD = MAX_FORWARD * 0.66f + MAX_FORWARD * 0.34f * speed;
   float M_LEFT = MAX_LEFT * 0.66f + MAX_LEFT * 0.34f * speed;
   float M_TURN = MAX_TURN * 0.66f + MAX_TURN * 0.34f * speed;

   float clampedForward = crop(forward, -M_FORWARD, M_FORWARD);
   float clampedLeft = crop(left, -M_LEFT, M_LEFT);
   float clampedTurn = crop(turn, -M_TURN, M_TURN);

   // Values in range [-1..1]
   float forwardAmount = clampedForward / M_FORWARD;
   float leftAmount = clampedLeft / M_LEFT;
   float turnAmount = clampedTurn / M_TURN;

   float x = fabs(forwardAmount);
   float y = fabs(leftAmount);
   float z = fabs(turnAmount);

   // see if the point we are given is already inside the allowed walk params volume
   if (evaluateWalkVolume(x, y, z) > 1.0)
   {
      float scale = 0.5;
      float high = 1.0;
      float low = 0.0;

      // This is basically a binary search to find the point on the surface.
      for (unsigned i = 0; i < 10; i++)
      {
         // give priority to turn. keep it the same
         x = fabs(forwardAmount) * scale;
         y = fabs(leftAmount) * scale;

         if (evaluateWalkVolume(x, y, z) > 1.0) {
            float newScale = (scale + low) / 2.0f;
            high = scale;
            scale = newScale;
         } else {
            float newScale = (scale + high) / 2.0f;
            low = scale;
            scale = newScale;
         }
      }

      forwardAmount *= scale;
      leftAmount *= scale;
   }

   forward = M_FORWARD * forwardAmount;
   left = M_LEFT * leftAmount;
   turn = clampedTurn;
}

float MIN(int x, int y)
{
  return  (float)( x > y ? y : x);
}
float MIN(float x, float y)
{
  return  ( x > y ? y : x);
}
float MIN(int x, float y)
{
  return  ( x > y ? y : x);
}
float MIN(float x, int y)
{
  return  ( x > y ? y : x);
}
float MAX(int x, int y)
{
  return  ( x > y ? x : y);
}
float MAX(float x, float y)
{
  return  ( x > y ? x : y);
}
float MAX(int x, float y)
{
  return  ( x > y ? x : y);
}
float MAX(float x, int y)
{
  return  ( x > y ? x : y);
}
// end of small functions

MAKE_MODULE(WalkingEngine, motionControl)

thread_local WalkingEngine* WalkingEngine::theInstance = 0;

WalkingEngine::WalkingEngine() :
  jointRequest(),
  active(ActionCommand::Body::NONE),
  filterSensor(gyrXOffset,
    gyrYOffset,
    angleXOffset,
    angleYOffset,
    bodyPitchOffset),
  t(0.0), z(0.0)
{
  theInstance = this;

  // init() is not called automatically when parameters are first read,
  // because this object has not been constructed at that time. So
  // it has to be called manually.
  init();

  // reset internal state
  reset();
}

bool WalkingEngine::handleMessage(InMessage& message)
{
  WalkingEngine* engine = theInstance;
  if(engine && message.getMessageID() == idWalkingEngineKick)
  {
    unsigned int id, size;
    message.bin >> id >> size;
    ASSERT(id < WalkRequest::numOfKickTypes);
    char* buffer = new char[size + 1];
    message.bin.read(buffer, size);
    buffer[size] = '\0';
    engine->kicks.load(WalkRequest::KickType(id), buffer);
    delete[] buffer;
    return true;
  }
  return false;
}

void WalkingEngine::init()
{
  dt = 0.01f;                                             // 100 Hz motion thread
  t = 0.0;                                               // initialise timers (in seconds)
  timer = 0.0;                                           // timer to crouch to walking height
  globalTime = 0;                                        // use for diagnostic purposes only
  T = BASE_WALK_PERIOD;                                  // seconds - the period of one step in a two step walk cycle
  stopping = false;                                      // legacy code for stopping robot?
  stopped = true;                                        // legacy code for stopped robot?
  leftL = leftR = lastLeft = left = 0.0f;                 // Side-step for left, right foot, and (last) left command in meters
  turnRL = turnRL0 = 0;                                  // Initial turn variables for feet
  forwardL  = forwardR  = 0.0;                           // forward step per for left and right foot in meters
  forwardR0 = forwardL0 = 0;                             // initial last positions for left and right feet keep constant during next walk step
  forward = lastForward = 0.0;                           // Current and previous forward value
  shoulderPitchL = shoulderPitchR = 0;                   // arm swing while walking
  shoulderRollL  = shoulderRollR  = 0;                   // Not used yet
  hiph = hiph0 = STAND_HIP_HEIGHT;                       // make robot stand initially based on Stand command
  foothL = foothR = 0;                                   // robots feet are both on the ground initially
  const float LeftThighLength = 100.00;                  // @Carrol: left thigh length is longer than the right one
  const float RightThighLength = 100.00;
  const float TibiaLength = 102.90f;
  const float FootHeight = 45.19f;
  thighL = LeftThighLength/MM_PER_M;                     // thigh length in meters
  thighR = RightThighLength/MM_PER_M;                    // thigh length in meters
  tibia = TibiaLength/MM_PER_M;                          // tibia length in meters
  ankle = FootHeight/MM_PER_M;                           // height of ankle above ground
  nextFootSwitchT = 0.0;                                 // next time-point to switch support foot (in seconds)
  stiffness = 0.9f;                                       // initial motor stiffness
  walkOption = NONE;                                     // initial walk 2014 option
  walkState = NOT_WALKING;                               // initial walkState
  supportFoothasChanged = false;                         // triggers support foot change actions
  comOffset = 0;                                         // Center of Mass offset in sagittal plane used to spread weight along feet in x-dir
  prevTurn = prevForwardL = prevForwardR = 0;            // odometry
  prevLeftL = prevLeftR = 0;                             // odometry
  exactStepsRequested = false;
  filteredGyroY = 0;
  filteredGyroX = 0;

  // Kick specific
  kickT = 0;
  kickPhase = MAX_KICK_PHASE;
  rock = 0;
  kneePitchL = kneePitchR = lastKneePitch = 0;
  lastShoulderPitchAdjust = 0;
  anklePitchL = anklePitchR = 0;
  lastKickForward = 0;
  lastSide = 0;
  lastKickTime = T;
  dynamicSide = 0.0f;
  turnAngle = 0;
  lastTurn = 0;
  lastTurn = 0;
  lankleRollAdjust = rankleRollAdjust = 0.f;
  motionOdometry.reset();

  actionFlag = 0;
  kickFinish = false;

  // FlashKick specific
  flashKickT = 0;
  flashKickPhase = MAX_FLASH_KICK_PHASE;
  rockLeft = rockRight = 0;
  lastFlashKickTime = T;
  hipPitchL = hipPitchR = 0;
  shoulderPitchAdjustL = shoulderPitchAdjustR = 0;

  //dribble engine
  DribbleFoot = ActionCommand::Body::LEFT;
  dribbleState = DribbleEND;
  TimeWhenLastDribbleEnd = 0;

  //lineup engine
  LineUpFoot = ActionCommand::Body::LEFT;
  LineUpHasStarted = false;
}

void WalkingEngine::reset()
{
//  active.actionType = ActionCommand::Body::STAND;
}

void WalkingEngine::update(WalkingEngineOutput& walkingEngineOutput)
{
//	std::cout<<"theInertialSensorData---->"<<theInertialSensorData.gyro.x() * 57.f*2.35f<<","<<theInertialSensorData.gyro.y() * 57.f*2.35f<<std::endl;
  // cout << "motion---> " << theMotionInfo.motion << "  kickType---> "  << theMotionInfo.walkRequest.kickType << "  kickFinish--->" << kickFinish << endl;
//	std::cout<<"walkOption------>"<<walkOption<<std::endl;
//	std::cout<<"walkState------>"<<walkState<<std::endl;
  lastOption = walkOption;
  JointValues tmpp;
  sensors.getSensorValues(theFsrSensorData, theInertialSensorData, theJointAngles);
  SensorValues tmp = sensors;
  //    std::cout<<"sensor00---->"<<sensors.sensors[Sensors::InertialSensor_GyrX]<<","<<sensors.sensors[Sensors::InertialSensor_GyrY]<<std::endl;
  sensors = filterSensor.getSensors(kinematics, sensors);
  //	std::cout<<"sensor11---->"<<sensors.sensors[Sensors::InertialSensor_GyrX]<<","<<sensors.sensors[Sensors::InertialSensor_GyrY]<<std::endl;
  kinematics.setSensorValues(sensors);
  kinematics.updateDHChain();
  bodyModel.updateZMPL(sensors);
  if(theMotionSelection.ratios[MotionRequest::walk] > 0.f || theMotionSelection.ratios[MotionRequest::stand] > 0.f)
  {

    updateMotionRequest();

    if(active.actionType == ActionCommand::Body::STAND)
      odometry.clear();
    JointValues j = makeJoints(theBallModel.estimate.position.x(), 0.f);
    odometrySum.turn += odometry.turn;
    odometrySum.turn = Angle(odometrySum.turn).normalize();
    odometrySum.forward += odometry.forward * cos(odometrySum.turn)
        - odometry.left * sin(odometrySum.turn);
    odometrySum.left += odometry.forward * sin(odometrySum.turn)
        + odometry.left * cos(odometrySum.turn);
//    odometrySum.forward += odometry.forward;
//    odometrySum.left += odometry.left;
    generateJointRequest(j);
    generateOutput(walkingEngineOutput);
    tmpp = j;
  }
  else
  {
    reset();
    generateDummyOutput(walkingEngineOutput);
  }
  PLOT("module:WalkingEngine:hippitch", RAD2DEG(tmpp.angles[Joints::lHipPitch]));
  PLOT("module:WalkingEngine:anklepitch", RAD2DEG(tmpp.angles[Joints::lAnklePitch]));
  PLOT("module:WalkingEngine:kneepitch", RAD2DEG(tmpp.angles[Joints::lKneePitch]));
  PLOT("module:WalkingEngine:filteredgyro", (sensors.sensors[Sensor::InertialSensor_GyrY]));
  PLOT("module:WalkingEngine:rawgyro", (tmp.sensors[Sensor::InertialSensor_GyrY]));
}

void WalkingEngine::updateMotionRequest()
{
  // get requested motion state
  active.actionType = ActionCommand::Body::STAND;
  active.forward = 0.f;
  active.left = 0.f;
  active.turn = 0.f;
  soft = false;
//  std::cout<<"theMotionRequest.motion------->"<<theMotionRequest.motion<<std::endl;
//  std::cout<<"theMotionSelection---->"<<theMotionSelection.ratios[MotionRequest::walk]<<std::endl;
//  std::cout<<"theMotionSelection---->"<<theMotionSelection.ratios[MotionRequest::stand]<<std::endl;
//  std::cout<<"theMotionSelection---->"<<theMotionSelection.ratios[MotionRequest::kick]<<std::endl;
  if(theGroundContactState.contact && theMotionRequest.motion == MotionRequest::walk)//if(theGroundContactState.contact && theMotionSelection.ratios[MotionRequest::walk] >= 1.f)
  {
//	  std::cout<<"theMotionSelection = walk"<<std::endl;
    if (theMotionRequest.motion == MotionRequest::walk)
    {
      active.actionType = ActionCommand::Body::WALK;
      active.bend = 1;
      // get the requested walk speed
      if (theMotionRequest.walkRequest.mode == WalkRequest::speedMode)
      {
//    	  LineUpReset();
    	  exactStepsRequested = false;
        active.forward = crop(theMotionRequest.walkRequest.speed.translation.x(), -MAX_FORWARD*1000, MAX_FORWARD*1000);
        active.left = crop(theMotionRequest.walkRequest.speed.translation.y(), -MAX_LEFT*1000, MAX_LEFT*1000);
        active.turn = crop(theMotionRequest.walkRequest.speed.rotation, -MAX_TURN, MAX_TURN);
      }
      else if(theMotionRequest.walkRequest.mode != WalkRequest::dribbleMode)
      {
//    	  LineUpReset();
    	  exactStepsRequested = false;
        active.forward = crop(theMotionRequest.walkRequest.speed.translation.x(), -1.f, 1.f) * MAX_FORWARD * 1000;
        active.left = crop(theMotionRequest.walkRequest.speed.translation.y(), -1.f, 1.f) * MAX_LEFT * 1000;
        active.turn = crop(theMotionRequest.walkRequest.speed.rotation, -1.f, 1.f) * MAX_TURN;
      }

      // get requested walk target
      if (theMotionRequest.walkRequest.mode == WalkRequest::targetMode)
      {
        soft = theMotionRequest.walkRequest.soft;
        flash = theMotionRequest.walkRequest.flash;
        if (theMotionRequest.walkRequest.kickType == WalkRequest::none)
        {
          lastKickType = -1;
          if (theMotionRequest.walkRequest.target != Pose2f()
              && theMotionRequest.walkRequest.target != lastCopiedWalkTarget)
          {
            lastCopiedWalkTarget = requestedWalkTarget = theMotionRequest
                .walkRequest.target;
            requestedWalkTarget.rotation = requestedWalkTarget.rotation.normalize();
//          targetRadian = atan2(requestedWalkTarget.translation.y(), requestedWalkTarget.translation.x());
            odometrySum.clear();

            actionFlag = 1;
          }
          float deltaX = (requestedWalkTarget.translation.x() - odometrySum.forward);
          float deltaY = (requestedWalkTarget.translation.y() - odometrySum.left);
          float x = cos(odometrySum.turn) * deltaX + sin(odometrySum.turn) * deltaY;
          float y = -sin(odometrySum.turn) * deltaX + cos(odometrySum.turn) * deltaY;
          // decide turn
          Angle radian = Angle(requestedWalkTarget.rotation - odometrySum.turn).normalize();

          if (abs(radian) < 10_deg) active.turn = radian;
          else if (radian < -10_deg) active.turn = - active.turn;
          // decide forward
          if (abs(x) < 100) active.forward = x;
          else if (x < -100) active.forward = -active.forward;
          // decide left
          if (abs(y) < 100) active.left = y;
          else if (y < -100) active.left = -active.left;

          if(abs(radian) < 10_deg) actionFlag = 0;
        }
      } // end of target mode
      if (theMotionRequest.walkRequest.mode == WalkRequest::pointMode)
      {
    	  active.forward = theMotionRequest.walkRequest.step.translation.x();
		    active.left = theMotionRequest.walkRequest.step.translation.y();
		    active.turn = theMotionRequest.walkRequest.step.rotation;
		    std::cout<<"forward--->"<<active.forward<<", left---->"<<active.left<<", turn---->"<<active.turn<<std::endl;
      }
      if(theMotionRequest.walkRequest.mode == WalkRequest::dribbleMode)
      {
    	  active.forward = 0;
    	  active.left = 0;
    	  active.turn = theMotionRequest.walkRequest.step.rotation;
//    	  std::cout<<"active.turn---->"<<active.turn<<std::endl;

    	  // std::cout<<"ok1"<<std::endl;
    	  if(Time::getTimeSince(TimeWhenLastDribbleEnd) < 400)
		  {
    		  // std::cout<<"ok2"<<std::endl;
			  toWalkRequest();
			  active.actionType = ActionCommand::Body::WALK;
			  active.forward = 1;
			  active.left = 0;
			  active.turn = 0;
		  }
    	  // std::cout<<"isKicking------>"<<isKicking<<std::endl;
    	  if(!isKicking)
    	  {
			  if(!DribbleHasEnded())
			  {
				  // std::cout<<"ok3"<<std::endl;
				  DribblePreProcess();
				  active.actionType = ActionCommand::Body::DRIBBLE;
			  }
			  else
			  {
				  // std::cout<<"ok4"<<std::endl;
				  DribbleReset();
				  if(!LineUpHasStarted)
				  {
					  ActionCommand::Body::Foot dribbleFoot;
					  if(theMotionRequest.DribbleFoot)
					  {
						  dribbleFoot = ActionCommand::Body::RIGHT;
					  }
					  else
					  {
						  dribbleFoot = ActionCommand::Body::LEFT;
					  }
					  LineUpStart(dribbleFoot);
				  }
				  if(!LineUpHasEnded())
				  {
					  // std::cout<<"ok5"<<std::endl;
					  LineUpPreProcess();
					  active.actionType = ActionCommand::Body::LINE_UP;
				  }
				  else
				  {
					  // std::cout<<"dribble"<<std::endl;
					  ActionCommand::Body::Foot dribbleFoot;
					  if(theMotionRequest.DribbleFoot)
					  {
						  dribbleFoot = ActionCommand::Body::RIGHT;
					  }
					  else
					  {
						  dribbleFoot = ActionCommand::Body::LEFT;
					  }
					  LineUpReset();
					  if(theMotionRequest.walkRequest.kickType == WalkRequest::none)
					  {
						  DribbleStart(dribbleFoot);
						  DribblePreProcess();
						  active.actionType = ActionCommand::Body::DRIBBLE;
					  }
					  else
					  {
						  isKicking = true;
					  }
				  }
			  }
    	  }
    	  else if(theMotionRequest.walkRequest.kickType != WalkRequest::none)
    	  {
    		  toKickRequest();
    	  }
      }
    }  // end of theMotionRequest.motion == MotionRequest::walk
  } // end of if walk
  else if(theMotionRequest.motion == MotionRequest::stand)
  {
//	std::cout<<"stand"<<std::endl;
    active.actionType = ActionCommand::Body::WALK;
    active.forward = active.left = active.turn = 0;
    active.bend = theMotionRequest.bend;
  }
//  std::cout<<"forward1111--->"<<active.forward<<", left---->"<<active.left<<", turn---->"<<active.turn<<std::endl;
//  std::cout<<"active.bend----->"<<active.bend<<std::endl;
}

void WalkingEngine::generateJointRequest(JointValues& j)
{
  // limit angles
  for (unsigned i = 0; i < Joints::numOfJoints; ++i) {
    if (isnan(j.angles[i]))
        j.angles[i] = 0;
    if (j.angles[i] < Radians::MinAngle[i])
      j.angles[i] = Radians::MinAngle[i];
    else if (j.angles[i] > Radians::MaxAngle[i])
      j.angles[i] = Radians::MaxAngle[i];
  }

  //head
  jointRequest.angles[Joints::headYaw] = theHeadJointRequest.pan == JointAngles::off ? Angle(0.f) : theHeadJointRequest.pan;
  jointRequest.angles[Joints::headPitch] = theHeadJointRequest.tilt == JointAngles::off ? Angle(0.f) : theHeadJointRequest.tilt;

  //arms
  jointRequest.angles[Joints::lShoulderPitch] = j.angles[Joints::lShoulderPitch];
  jointRequest.angles[Joints::lShoulderRoll] = j.angles[Joints::lShoulderRoll];
  jointRequest.angles[Joints::lElbowYaw] = j.angles[Joints::lElbowYaw];
  jointRequest.angles[Joints::lElbowRoll] = j.angles[Joints::lElbowRoll];
  jointRequest.angles[Joints::lWristYaw] = j.angles[Joints::lWristYaw];
  jointRequest.angles[Joints::lHand] = j.angles[Joints::lHand];

  jointRequest.angles[Joints::rShoulderPitch] = j.angles[Joints::rShoulderPitch];
  jointRequest.angles[Joints::rShoulderRoll] = j.angles[Joints::rShoulderRoll];
  jointRequest.angles[Joints::rElbowYaw] = j.angles[Joints::rElbowYaw];
  jointRequest.angles[Joints::rElbowRoll] = j.angles[Joints::rElbowRoll];
  jointRequest.angles[Joints::rWristYaw] = j.angles[Joints::rWristYaw];
  jointRequest.angles[Joints::rHand] = j.angles[Joints::rHand];

  //legs
  jointRequest.angles[Joints::lHipYawPitch] = j.angles[Joints::lHipYawPitch];
  jointRequest.angles[Joints::lHipRoll] = j.angles[Joints::lHipRoll];
  jointRequest.angles[Joints::lHipPitch] = j.angles[Joints::lHipPitch];
  jointRequest.angles[Joints::lKneePitch] = j.angles[Joints::lKneePitch];
  jointRequest.angles[Joints::lAnklePitch] = j.angles[Joints::lAnklePitch];
  jointRequest.angles[Joints::lAnkleRoll] = j.angles[Joints::lAnkleRoll];

  jointRequest.angles[Joints::rHipYawPitch] = j.angles[Joints::lHipYawPitch];
  jointRequest.angles[Joints::rHipPitch] = j.angles[Joints::rHipPitch];
  jointRequest.angles[Joints::rHipRoll] = j.angles[Joints::rHipRoll];
  jointRequest.angles[Joints::rKneePitch] = j.angles[Joints::rKneePitch];
  jointRequest.angles[Joints::rAnklePitch] = j.angles[Joints::rAnklePitch];
  jointRequest.angles[Joints::rAnkleRoll] = j.angles[Joints::rAnkleRoll];

  // stiffness
  for (uint8_t i = 0; i < Joints::numOfJoints; ++i)
    j.stiffnesses[i] = j.stiffnesses[i]*100;
  jointRequest.stiffnessData.stiffnesses[Joints::lShoulderPitch] = j.stiffnesses[Joints::lShoulderPitch];
  jointRequest.stiffnessData.stiffnesses[Joints::lShoulderRoll] = j.stiffnesses[Joints::lShoulderRoll];
  jointRequest.stiffnessData.stiffnesses[Joints::lElbowYaw] = j.stiffnesses[Joints::lElbowYaw];
  jointRequest.stiffnessData.stiffnesses[Joints::lElbowRoll] = j.stiffnesses[Joints::lElbowRoll];
  jointRequest.stiffnessData.stiffnesses[Joints::lWristYaw] = j.stiffnesses[Joints::lWristYaw];
  jointRequest.stiffnessData.stiffnesses[Joints::lHand] = j.stiffnesses[Joints::lHand];
  jointRequest.stiffnessData.stiffnesses[Joints::rShoulderPitch] = j.stiffnesses[Joints::rShoulderPitch];
  jointRequest.stiffnessData.stiffnesses[Joints::rShoulderRoll] = j.stiffnesses[Joints::rShoulderRoll];
  jointRequest.stiffnessData.stiffnesses[Joints::rElbowYaw] = j.stiffnesses[Joints::rElbowYaw];
  jointRequest.stiffnessData.stiffnesses[Joints::rElbowRoll] = j.stiffnesses[Joints::rElbowRoll];
  jointRequest.stiffnessData.stiffnesses[Joints::rWristYaw] = j.stiffnesses[Joints::rWristYaw];
  jointRequest.stiffnessData.stiffnesses[Joints::rHand] = j.stiffnesses[Joints::rHand];

  jointRequest.stiffnessData.stiffnesses[Joints::lHipYawPitch] = j.stiffnesses[Joints::lHipYawPitch];
  jointRequest.stiffnessData.stiffnesses[Joints::lHipRoll] = j.stiffnesses[Joints::lHipRoll];
  jointRequest.stiffnessData.stiffnesses[Joints::lHipPitch] = j.stiffnesses[Joints::lHipPitch];
  jointRequest.stiffnessData.stiffnesses[Joints::lKneePitch] = j.stiffnesses[Joints::lKneePitch];
  jointRequest.stiffnessData.stiffnesses[Joints::lAnklePitch] = j.stiffnesses[Joints::lAnklePitch];
  jointRequest.stiffnessData.stiffnesses[Joints::lAnkleRoll] = j.stiffnesses[Joints::lAnkleRoll];
  jointRequest.stiffnessData.stiffnesses[Joints::rHipYawPitch] = j.stiffnesses[Joints::lHipYawPitch];
  jointRequest.stiffnessData.stiffnesses[Joints::rHipPitch] = j.stiffnesses[Joints::rHipPitch];
  jointRequest.stiffnessData.stiffnesses[Joints::rHipRoll] = j.stiffnesses[Joints::rHipRoll];
  jointRequest.stiffnessData.stiffnesses[Joints::rKneePitch] = j.stiffnesses[Joints::rKneePitch];
  jointRequest.stiffnessData.stiffnesses[Joints::rAnklePitch] = j.stiffnesses[Joints::rAnklePitch];
  jointRequest.stiffnessData.stiffnesses[Joints::rAnkleRoll] = j.stiffnesses[Joints::rAnkleRoll];
  jointRequest.stiffnessData.stiffnesses[Joints::headPitch] = 100;
  jointRequest.stiffnessData.stiffnesses[Joints::headYaw] = 100;

  // turn head joints off?
  if(theHeadJointRequest.pan == JointAngles::off)
    jointRequest.angles[Joints::headYaw] = JointAngles::off;
  if(theHeadJointRequest.tilt == JointAngles::off)
    jointRequest.angles[Joints::headPitch] = JointAngles::off;
}

JointValues WalkingEngine::makeJoints(float ballX, float ballY)
{
//	std::cout<<"hiph----->"<<hiph<<std::endl;
  float KICK_LEAN = KICK_LEAN_V5;
  // 0. The very first time walk is called, the previous stand height could have been anything, so make sure we interpolate from that
  if (walkOption == NONE)
  {
    // Calculate the current hip height by checking how bent the knee is
    //sensors.getSensorValues();
//    hiph = sensors.joints.angles[Joints::lKneePitch] / KNEE_PITCH_RANGE
//        * (WALK_HIP_HEIGHT - STAND_HIP_HEIGHT) + STAND_HIP_HEIGHT;
    hiph = STAND_HIP_HEIGHT;
  }

  // 1. Read in new walk values (forward, left, turn, power) only at the start of a walk step cycle, ie when t = 0
//  std::cout<<"t-------->"<<t<<std::endl;
  if (t == 0 && walkOption != STEP)
  {
    //WALK_HIP_HEIGHT = WALK_HIP_HEIGHT;
    // active = request->body;
    lastForward = forward;                           // back up old value in m/s
    lastLeft = left;  // used to detect when the left walk parameter changes sign
    forward = (float) active.forward / MM_PER_M;    // in meters
    left = (float) active.left / MM_PER_M;       // in meters
    turn = active.turn;                       // in radians
    power = active.power;  // controls stiffness when standing and kicking when walking*
    bend = active.bend;                       // knee-bend parameter
    speed = active.speed;    // used to distinguish between jabKick and walkKick
    foot = active.foot;                       // kicking foot
    isFast = active.isFast;

    if (stopping)
    {                                    // not used at present
    }
    else
    {
      stopped = false;                                  // (re)activate
    }
    // 1.0 For backwards compatibility with old interface (can be deleted when behaviours are updated)
    if (forward == 0 and left == 0 and turn == 0 and power == 0)
      bend = 0;
    // 1.1 Scale back values to try to ensure stability.
    //    for (int i = 0; i < cJoints::numOfJoints; ++i) {
    //      float temp = sensors.joints.temperatures[i];
    //        if(temp > 70)
    //          speed = MIN(0.5,speed);
    //        if(temp > 75)
    //          speed = MIN(0.5,speed);
    //    }
    if (!exactStepsRequested)
    {
      ellipsoidClampWalk(forward, left, turn, speed);
    }
    // Carrol  : not actually use at present
    float f = forward * MM_PER_M;
    float l = left * MM_PER_M;
    avoidFeet(f, l, turn);
    forward = f /MM_PER_M;
    left = l / MM_PER_M;

    // 1.2 Modify T when sidestepping and turning
    T = BASE_WALK_PERIOD + 0.1 * abs(left) / MAX_LEFT;

    // 1.3 jabKick
    //if(isFast and speed==0) cout << "time = "<< globalTime << " jabKick " << foot << endl;

    // 1.4 walkKick @yongqi: found out it kind of useless
    if(soft)
      walkKick(ballX, bodyModel);

    // 1.5 ratchet forward by FORWARD_CHANGE
    if (!exactStepsRequested)
    {
      if (abs(forward - lastForward) > FORWARD_CHANGE)
      {                // ie greater than a FORWARD_CHANGE / sec change
        forward = lastForward
            + (forward - lastForward) / abs(forward - lastForward)
                * FORWARD_CHANGE;   //  + / - 上限
      }
      if (abs(left - lastLeft) > LEFT_CHANGE)
      {
        left = lastLeft
            + (left - lastLeft) / abs(left - lastLeft) * LEFT_CHANGE;
      }
    }
    // 1.6 Walk Calibration
    // The definition of forward, left and turn is the actual distance/angle traveled in one second
    // One walk-cycle consists of two Phases, a left phase (left swing foot) and a right phase (right swing foot)

    if (!exactStepsRequested)
    {
      forward *= 2 * T;  // theoretical calibration. 2 - because there are two steps per walk cycle
      left *= 2 * T;
      turn *= 2 * T;
      // linear calibration to achieve actual performance ie turn in action command achieves turn/sec in radians on the real robot
      forward *= 1.0;
      left *= 0.82;
      turn *= 1.43;
    }
    // ？？ 为啥反向
    turn *= -1;   // reverses sign
  }
//  std::cout<<"forward---->"<<forward<<std::endl;

  // 2. Update timer
  t += dt;
  globalTime += dt;
  lastKickTime += dt;
  lastFlashKickTime += dt;

  // 3. Determine Walk2014 Option
  if (active.actionType != ActionCommand::Body::KICK && active.actionType != ActionCommand::Body::REF_PICKUP)
  {
    kickFinish = false;
  }

  // 3. Determine Walk2014 Option
  if (active.actionType != ActionCommand::Body::KICK && (kickT > 0 || flashKickT > 0)
      && active.actionType != ActionCommand::Body::REF_PICKUP)
  {
//	  std::cout<<"REF_PICKUP--------------------------------------"<<std::endl;
    // we want to stop kicking, but also don't let ref pick up take over during kick **HACK ALERT** this is particularly for mario whose foot sensor dies while kicking
    // Finish transition out if in the middle of a kick by skipping to the end phase
    if (canAbbortKick())
    {
      kickT = BACK_PHASE + kickPhase + THROUGH_PHASE;
    }
  }
  else if (active.actionType == ActionCommand::Body::KICK)
  {
//	  std::cout<<"kicking--------------------------------------"<<std::endl;
    // This makes sure that the action type gets set back to walk just after a kick is finished.
    // If we don't leave enough time for this to happen, motion moves back into a kick before behaviour
    // can change its mind.
    if ((lastKickTime < 2 * T) || (lastFlashKickTime < 2 * T))
    {
      active.actionType = ActionCommand::Body::WALK;
    }
    else if (abs(hiph - WALK_HIP_HEIGHT) < .0001)
    {                  // make sure we retain the designated walking height
      if (walkOption == WALK || walkOption == STEP)
      {
        // make sure walk is in neutral stance before kicking, L is symmetrical to R
        // cout << "prepKick" << fabs(forwardL) << " " << fabs(leftL) << " " << turnRL << " " << t << endl;
        if (fabs(forwardL) < EPSILON && fabs(leftL) < EPSILON
            && fabs(turnRL) < TURN_EPSILON && t == dt)
        {
          // Assuming already at t = 0 from active getting set to kick
          // Any new settings the first time walkOption==KICK go here

          // Calculate if turn step is required before kicking
          if (walkOption
              == WALK&& fabs(active.kickDirection) >= TURN_THRESHOLD)
          {
            // Halve the kick direction since HypL and HypR are connected
            turnAngle = MIN(DEG2RAD(90), fabs(active.kickDirection)) / 2;  // Safe max value for Hyp is ~45
            if ((active.foot == ActionCommand::Body::LEFT
                && !bodyModel.isLeftPhase)
                || (active.foot != ActionCommand::Body::LEFT
                    && bodyModel.isLeftPhase))
            {
              walkOption = STEP;
            }

            // If no turn step is required, kick
          }
          else if (fabs(active.kickDirection) < TURN_THRESHOLD)
          {
            // prep kick when the other foot is lifted
            if (active.foot == ActionCommand::Body::LEFT
                && !bodyModel.isLeftPhase)
            {
              prepKick(true, bodyModel);
            }
            else if (active.foot != ActionCommand::Body::LEFT
                && bodyModel.isLeftPhase)
            {
              prepKick(false, bodyModel);
            }
          }
        }
      }
      else if (walkOption != KICK)
      {
        // Calculate if turn is required before kicking
        if (fabs(active.kickDirection) >= TURN_THRESHOLD)
        {
          turnAngle = MIN(DEG2RAD(90), fabs(active.kickDirection)) / 2;  // Safe max value for Hyp is ~45
          walkOption = STEP;
        }
        else
        {
          prepKick(active.foot == ActionCommand::Body::LEFT, bodyModel);
        }
      }
    }
    else
    {                                               // hiph not= walkHipHeight
      if (walkOption != CROUCH)
      {  // robot starts crouching to walking height
        hiph0 = hiph;
        timer = 0;
      }
      walkOption = CROUCH;                        // continue crouching
    }
  }  // end Kick test
  else if (walkOption == WALK and walkState != NOT_WALKING)
  {
//	  std::cout<<"walking--------------------------------------"<<std::endl;
	  if(forward == 0 && left == 0 && turn == 0)
	  {
		  walkState = STOPPING;
	  }
  }
  else
  {

//	  std::cout<<"standing--------------------------------------"<<std::endl;
	  if(bend == 0)
	  {
//		  std::cout<<"bend------->"<<bend<<std::endl;
//		  std::cout<<"hiph------->"<<hiph-STAND_HIP_HEIGHT<<std::endl;
		  if(abs(hiph - STAND_HIP_HEIGHT) < 0.0001)
		  {
//			  std::cout<<"walkoption = stand"<<std::endl;
			  walkOption = STAND;
		  }
		  else
		  {
//			  std::cout<<"standup-----------------"<<std::endl;
			  if(walkOption != STANDUP)
			  {
				  hiph0 = hiph;
				  timer = 0;
			  }
			  walkOption = STANDUP;
		  }
	  }
	  else if(forward  == 0 && left == 0 && turn == 0 && bend == 1)
	  {
		  if(abs(hiph - STAND_HIP_HEIGHT < 0.0001))
		  {
			  walkOption = READY;
		  }
		  else
		  {
			  if(walkOption != CROUCH)
			  {
				  hiph0 = hiph;
				  timer = 0;
			  }
			  walkOption = CROUCH;
		  }
	  }
	  else
	  {
		  if(abs(hiph - WALK_HIP_HEIGHT < 0.0001))
		  {
			  if(walkOption != WALK)
			  {
				  walkState = STARTING;
				  nextFootSwitchT = T;
			  }
			  walkOption = WALK;
		  }
		  else
		  {
			  if(walkOption != CROUCH)
			  {
				  hiph0 = hiph;
				  timer = 0;
			  }
			  walkOption = CROUCH;
		  }
	  }
  }
//  std::cout<<"option--> "<<walkOption<<std::endl;
//  std::cout<<"state--> "<<walkState<<endl;
  // 4. Execute Walk2014 Option
  if (walkOption == STAND)
  {                         // Place CoM over ankle and turn set power to motors
//    cout<<"stand\n";
    hiph = STAND_HIP_HEIGHT;
    forward = left = turn = 0;
    t = nextFootSwitchT = 0;
    stiffness = power;
    if (stiffness < 0.2)
      stiffness = 0.2;
//    comOffset = 0;
    if (fabs(offsetVal) > 0.00001f)
      comOffset = offsetVal;
  }
  else if (walkOption == STANDUP)
  {
//    cout<<"stand up\n";
    hiph = hiph0
        + (STAND_HIP_HEIGHT - hiph0)
            * parabolicStep(timer, CROUCH_STAND_PERIOD, 0);
    forward = left = turn = 0;
    comOffset -= 0.02 * comOffset;  // reduce offset to zero to allow stiffness to be turned down
    stiffness = 1;
    t = nextFootSwitchT = 0;
    timer += dt;
  }
  else if (walkOption == CROUCH)
  {
//    cout<<"crouch \n";
    forward = left = turn = 0;
    stiffness = 1;
    hiph = hiph0
        + (WALK_HIP_HEIGHT - hiph0)
            * parabolicStep(timer, CROUCH_STAND_PERIOD, 0);
    comOffset = COM_OFFSET * parabolicStep(timer, CROUCH_STAND_PERIOD, 0);  // move comOffset to 0.01 meters when walking
    t = nextFootSwitchT = 0;
    timer += dt;                                        // inc. option timer
  }
  else if (walkOption == WALK)
  {
//    cout<<"walk \n";
//    if (fabs(offsetVal) > 0.00001f)
//      comOffset = offsetVal;
    stiffness = 1;
  }
  else if (walkOption == KICK)
  {
    stiffness = 1;
  }
  else if (walkOption == STEP)
  {
    stiffness = 1;
    nextFootSwitchT = TURN_PERIOD;
  }
  if (walkOption == READY)
  {
//    cout<<"ready\n";
    forward = left = turn = 0;
    stiffness = power;
    if (stiffness < 0.4)
      stiffness = 0.4;        // need enough stiffness to keep crouching posture
    t = nextFootSwitchT = 0;
  }

  // 5. Determine walk variables throughout the walk step phase
//  std::cout<<"T--> "<<nextFootSwitchT<<std::endl;
  if (walkOption == WALK and nextFootSwitchT > 0)
  {
    // 5.1 Calculate the height to lift each swing foot
    float maxFootHeight = BASE_LEG_LIFT + abs(forward) * 0.01
        + abs(left) * 0.02;
    // Carrol : 摆动脚在空中类似半个椭圆的轨迹
    float varfootHeight = maxFootHeight * parabolicReturn(t / nextFootSwitchT);  // 0.012 lift of swing foot from ground
//    std::cout<<"forward----->"<<forward<<std::endl;
    // 5.2 When walking in an arc, the outside foot needs to travel further than the inside one.
    // void
    // 5.3L Calculate intra-walkphase forward, left and turn at time-step dt, for left swing foot
    if (bodyModel.isLeftPhase)
    {                         // if the support foot is right
//      std::cout<<"isLeft------------------"<<std::endl;
      if (bodyModel.isLeftPhase != lastIsLeftPhase)
      {
        // 5.3.1L forward (the / by 4 is because the CoM moves as well and forwardL is wrt the CoM
        // 支撑脚
        forwardR = forwardR0
            + ((forward) / 4 - forwardR0) * linearStep(t, nextFootSwitchT);
        // 摆动脚
        forwardL = forwardL0
            + parabolicStep(t, nextFootSwitchT, 0)
                * (-(forward) / 4 - forwardL0);  // swing-foot follow-through
            // 5.3.3L Determine how much to lean from side to side - removed
            // 5.3.4L left
        if (left > 0)
        {
          leftR = leftAngle() / 2;     // share left between both feet, hence /2
          // if changing direction of left command, cancel swingAngle out of next left step
          if (lastLeft * left < 0)
            leftR -= swingAngle * (1 - parabolicStep(t, nextFootSwitchT, 0.1));
          leftL = -leftR;
        }
        else
        {
          leftL = swingAngle * (1 - parabolicStep(t, nextFootSwitchT, 0.0));
          leftR = -leftL;
        }
        // 5.3.5L turn
        if (turn < 0)
        {
          turnRL = turnRL0
              + (-.67 * turn - turnRL0)
                  * parabolicStep(t, nextFootSwitchT, 0.0);
        }
        else
        {
          turnRL = turnRL0
              + (-.33 * turn - turnRL0)
                  * parabolicStep(t, nextFootSwitchT, 0.0);  //turn back to restore previous turn angle
        }
      }
      // 5.3.6L determine how high to lift the swing foot off the ground
      foothL = varfootHeight;                         // lift left swing foot
      foothR = 0;                                   // do not lift support foot;
    }
    // 5.3R Calculate intra-walkphase forward, left and turn at time-step dt, for right swing foot
    if (not bodyModel.isLeftPhase)
    {                      // if the support foot is left
//      std::cout<<"isRight-------------------"<<endl;
      if (bodyModel.isLeftPhase != lastIsLeftPhase)
      {
//        std::cout<<"~~~~~~~~~~~~~~~~~~~~~~"<<endl;
        // 5.3.1R forward
        forwardL = forwardL0
            + ((forward) / 4 - forwardL0) * linearStep(t, nextFootSwitchT);
        forwardR = forwardR0
            + parabolicStep(t, nextFootSwitchT, 0)
                * (-(forward) / 4 - forwardR0);  // swing-foot follow-through
            // 5.3.2R Jab-Kick with right foot
            // 5.3.3R lean - not used
            // 5.3.4R left
        if (left < 0)
        {
          leftL = leftAngle() / 2;  // divide by 2 to share left between both feet
          // if changing direction of left command, cancel swingAngle out of next left step
          if (lastLeft * left < 0)
            leftL -= swingAngle * (1 - parabolicStep(t, nextFootSwitchT, 0.1));
          leftR = -leftL;
        }
        else
        {
          leftR = swingAngle * (1 - parabolicStep(t, nextFootSwitchT, 0.0));
          leftL = -leftR;
        }
        // 5.3.5R turn
        if (turn < 0)
        {
          turnRL = turnRL0
              + (.33 * turn - turnRL0) * parabolicStep(t, nextFootSwitchT, 0.0);
        }
        else
        {
          turnRL = turnRL0
              + (.67 * turn - turnRL0) * parabolicStep(t, nextFootSwitchT, 0.0);
        }
        // 5.3.6R Foot height
      }
//      cout<<"varfootHeight---> "<<varfootHeight<<endl;
      foothR = varfootHeight; // 0 - 0.011
      foothL = 0;
    }
    // 5.4 Special conditions when priming the walk
    if (walkState == STARTING)
    {
      turnRL = 0;   // don't turn on start of rocking - may relax this in future
      foothL /= 3.5;  // reduce max lift due to short duration - may need to adjust this later
      foothR /= 3.5;                                  // "
      leftR = leftL = 0;      // don't try to step left on starting and stopping
      forwardL = forwardR = 0;                 // don't move forward or backward
    }
    // 5.5 "natural" arm swing while walking/kicking to counterbalance foot swing
    shoulderPitchR = -forwardR * 6;  //10;                     // forwardR is in meters, 10 is an arbitrary scale factor to match previous walk
    shoulderPitchL = -forwardL * 6;                        //10;
  }

  else if (walkOption == KICK)
  {

   //flash kick
    if(flash)
    {
      if (active.foot == ActionCommand::Body::LEFT)
      {
        makeFlashKickJoints(FLASH_KICK_LEAN, FLASH_KICK_STEP_HEIGHT, FLASH_SHIFT_PERIOD, foothL, forwardL, leftL, kneePitchL, shoulderRollL, shoulderPitchAdjustL, anklePitchL, hipPitchL, ballY, &active);
        leftR = leftL * 0.1;  // Balance slightly more over support foot if need be
        lankleRollAdjust = 0.f;
        rankleRollAdjust = rankleRollAdjustRad;
        // hipPitchR = hipPitchL;
      }
      else
      {  // with added adjustments for right side
        makeFlashKickJoints(-(FLASH_KICK_LEAN), (FLASH_KICK_STEP_HEIGHT+0.005),
                              FLASH_SHIFT_PERIOD, foothR, forwardR, leftR, kneePitchR, shoulderRollR, shoulderPitchAdjustR,anklePitchR, hipPitchR, ballY, &active);
        leftR = -leftR;  // switch signs for right side
        leftL = leftR * 0.12;
        rankleRollAdjust = 0.f;
        lankleRollAdjust = lankleRollAdjustRad;
        // hipPitchL = hipPitchR;
      }
    }
    else if (!flash)
    {
      // Kicking
      if (active.foot == ActionCommand::Body::LEFT)
      {
        makeForwardKickJoints(KICK_LEAN, KICK_STEP_HEIGHT, SHIFT_PERIOD, foothL,
                              forwardL, leftL, kneePitchL, shoulderRollL, shoulderPitchAdjustL,
                              anklePitchL, ballY, &active);
        leftR = leftL * 0.1;  // Balance slightly more over support foot if need be
        lankleRollAdjust = 0.f;
        rankleRollAdjust = rankleRollAdjustRad;
      }
      else
      {  // with added adjustments for right side
        makeForwardKickJoints(-(KICK_LEAN + 2), KICK_STEP_HEIGHT + 0.00,
                              SHIFT_PERIOD, foothR, forwardR, leftR, kneePitchR,
                              shoulderRollR, shoulderPitchAdjustR, anklePitchR, ballY, &active);
        leftR = -leftR;  // switch signs for right side
        leftL = leftR * 0.12;
        rankleRollAdjust = 0.f;
        lankleRollAdjust = lankleRollAdjustRad;
      }
    }
    }

  else if (walkOption == STEP)
  {  // Turn step
    float footHeight = TURN_STEP_HEIGHT * parabolicReturn(t / nextFootSwitchT);
    if (active.foot != ActionCommand::Body::LEFT)
    {     // if the support foot is right while turning
      foothL = footHeight;                            // lift left swing foot
      foothR = 0;                                   // do not lift support foot;
      rock = TURN_LEAN * parabolicReturn(t / nextFootSwitchT);
    }
    else
    {                               // if the support foot is left while turning
      foothR = footHeight;                            // lift right swing foot
      foothL = 0;                                   // do not lift support foot;
      rock = -TURN_LEAN * parabolicReturn(t / nextFootSwitchT);
    }

    // Interpolate turn step over turnT with dead period to avoid dragging against the ground
    turnRL = turnAngle * TURN_SCALE * parabolicStep(t, nextFootSwitchT, 0.15);
    // Once the turn step has pretty much finished, readjust the kick direction
    if (t >= nextFootSwitchT - dt)
    {
      if (active.kickDirection >= TURN_THRESHOLD)
      {
        active.kickDirection -= (turnAngle * 2);
      }
      else if (active.kickDirection <= -TURN_THRESHOLD)
      {
        active.kickDirection += (turnAngle * 2);
      }
      // tjark
      t = 0;
      turnRL = 0;
    }
    lastTurn = turnRL;
  }

  else
  {  // When walk option is not WALK or KICK
    foothL = foothR = 0;
  }

  // 6. Changing Support Foot. Note bodyModel.isLeftPhase means left foot is swing foot.
  //    t>0.75*T tries to avoid bounce, especially when side-stepping
  //    lastZMPL*ZMPL<0.0 indicates that support foot has changed
  //    t>3*T tires to get out of "suck" situations
  //bodyModel.updateZMPL();
//  cout<<"t = "<<t<<endl;
//  cout<<"T = "<<T<<endl;
#ifdef TARGET_ROBOT
  if ((t > 0.75 * T and bodyModel.ZMPL * bodyModel.lastZMPL < 0) or t > 3 * T)  // Carrol : ZMP过零
#else
  if ((t > 0.75 * T and bodyModel.ZMPL * bodyModel.lastZMPL < 0) or t > T)  // Carrol : ZMP过零
#endif
  {
//    cout<<"changed"<<endl;
    supportFoothasChanged = true;
  }
  if (supportFoothasChanged)
  {
//    cout<<"supportFoothasC hanged"<<endl;
    lastIsLeftPhase = bodyModel.isLeftPhase;
#ifdef TARGET_ROBOT
    bodyModel.setIsLeftPhase(bodyModel.ZMPL < 0);  // set isLeft phase in body model for kinematics etc
#else
    bodyModel.setIsLeftPhase(!bodyModel.isLeftPhase);  // set isLeft phase in body model for kinematics etc
#endif
  }
  if (supportFoothasChanged and walkOption == WALK)
  {
    supportFoothasChanged = false;                      //reset

    // 6.1 Recover previous "left" swing angle
    if (bodyModel.isLeftPhase)
      swingAngle = leftL;
    else
      swingAngle = leftR;

    // 6.2 Decide on timing of next walk step phase
    if (walkState == NOT_WALKING)
    {                       // Start the walk
      nextFootSwitchT = T;
      walkState = STARTING;
    }
    else if (walkState == STARTING)
    {
      nextFootSwitchT = T;
      walkState = WALKING;
    }
    else if (walkState == WALKING)
    {
      nextFootSwitchT = T;
      walkState = WALKING;  // continue walking until interrupted by a command to stop (or kick?)
    }
    else if (walkState == STOPPING)
    {
      nextFootSwitchT = T;
      walkState = NOT_WALKING;
    }
    else
      cout << "Should never get here: walkState error" << endl;
    // 6.3 reset step phase time
    t = 0;                                             // reset step phase timer
    // 6.4 backup values
    turnRL0 = turnRL;                   // store turn value for use in next step
    forwardR0 = forwardR;                               // sagittal right foot
    forwardL0 = forwardL;                               // sagittal left foot
    // 6.5 Other stuff on support foot change
    // none at the moment
  }  // end of changing support foot

  // 7. Sagittal Balance
  filteredGyroY = 0.8 * filteredGyroY
      + 0.2 * sensors.sensors[InertialSensor_GyrY];
  balanceAdjustment = filteredGyroY / 25;  // adjust ankle tilt in proportion to filtered gryoY
  if (walkOption == READY)
    balanceAdjustment = 0;        // to stop swaying while not walking

  // 7.5 Coronal Balance
  filteredGyroX = 0.8 * filteredGyroX
      + 0.2 * sensors.sensors[InertialSensor_GyrX];
  coronalBalanceAdjustment = filteredGyroX / 25;  // adjust ankle roll in proportion to filtered gryoX

  // 8. Odometry update for localisation
  lastOdometry = odometry;
  odometry = updateOdometry(bodyModel.isLeftPhase);
  // @yongqi: correct odometry
  odometry.forward *= 1.1f;
  odometry.left *= 1.05f;
  odometry.turn *= 1.1f;
//  odometry.forward = odometry.forward
//      + motionOdometry.updateOdometry(sensors, updateOdometry(bodyModel.isLeftPhase)).forward;
//  odometry.left = odometry.left
//      + motionOdometry.updateOdometry(sensors, updateOdometry(bodyModel.isLeftPhase)).left;
//  odometry.turn = odometry.turn + motionOdometry.updateOdometry(sensors, updateOdometry(bodyModel.isLeftPhase)).turn;

  // Diagnostic Printouts - commented out for production code
  // cout << sensors.sensors[Sensors::InertialSensor_GyrY] << endl;
  //    cout << t <<" "<< leftL <<" "<< leftR << endl;

  // 9. Work out joint angles from walk variables above

  // 9.1 Left foot closed form inverse kinematics
  float leghL = hiph - foothL - ankle;  // vertical height between ankle and hip in meters
//  std::cout<<"hiph----->"<<hiph<<", foothl----->"<<foothL<<std::endl;
  float legX0L = leghL / cos(leftL);  // leg extension (eliminating knee) when forwardL = 0
  float legXL = sqrt(
      legX0L * legX0L + (forwardL + comOffset) * (forwardL + comOffset));  //leg extension at forwardL
  float beta1L = acos(
      (thighL * thighL + legXL * legXL - tibia * tibia) / (2.0f * thighL * legXL));  // acute angle at hip in thigh-tibia triangle
  float beta2L = acos(
      (tibia * tibia + legXL * legXL - thighL * thighL) / (2.0f * tibia * legXL));  // acute angle at ankle in thigh-tibia triangle
//std::cout<<"legXL---->"<<legXL<<", legX0L----->"<<legX0L<<", forwardL"<<forwardL<<"comOffset"<<comOffset<<std::endl;
  float tempL = legX0L / legXL;
  if (tempL > 1.0f)
    tempL = 1.0f;  // sin ratio to calculate leg extension pitch. If > 1 due to numerical error round down.
  float deltaL = asin(tempL);                             // leg extension angle
  float dirL = 1.0f;
  if ((forwardL + comOffset) > 0.0f)
    dirL = -1.0f;  // signum of position of foot
  float HpL = beta1L + dirL * (M_PI / 2.0f - deltaL);  // Hip pitch is sum of leg-extension + hip acute angle above
  float ApL = beta2L + dirL * (deltaL - M_PI / 2.0f);  // Ankle pitch is a similar calculation for the ankle joint
//  std::cout<<"beta2l--->"<<RAD2DEG(beta2L)<<", dirl----->"<<dirL<<", deltaL---->"<<RAD2DEG(deltaL)<<std::endl;
  float KpL = HpL + ApL;  // to keep torso upright with both feet on the ground, the knee pitch is always the sum of the hip pitch and the ankle pitch.
  // 9.2 right foot closed form inverse kinematics (comments as above but for right leg)
  float leghR = hiph - foothR - ankle;
  float legX0R = leghR / cos(leftR);
  float legXR = sqrt(
      legX0R * legX0R + (forwardR + comOffset) * (forwardR + comOffset));
  float dirR = 1.0f;
  if ((forwardR + comOffset) > 0.0f)
    dirR = -1.0f;
  float beta1R = acos(
      (thighR * thighR + legXR * legXR - tibia * tibia) / (2.0f * thighR * legXR));
  float beta2R = acos(
      (tibia * tibia + legXR * legXR - thighR * thighR) / (2.0f * tibia * legXR));
  float tempR = legX0R / legXR;
  if (tempR > 1.0f)
    tempR = 1.0f;
  float deltaR = asin(tempR);
  float HpR = beta1R + dirR * (M_PI / 2.0f - deltaR);
  float ApR = beta2R + dirR * (deltaR - M_PI / 2.0f);
  float KpR = HpR + ApR;
//  std::cout<<"kpr---->"<<KpR<<std::endl;
  // 9.3 Sert hip and ankle values
  float HrL = -leftL;
  float HrR = -leftR;
  float ArL = -HrL;
  float ArR = -HrR;
  if (walkOption == KICK || walkOption == STEP)
  {
      HrL += rock;
      HrR += rock;
      ArL -= rock;
      ArR -= rock;
  }

  // 9.4 Adjust HpL, HrL, ApL, ArL LEFT based on Hyp turn to keep ankle in situ
  // Turning
  XYZ_Coord tL = mf2b(z, -HpL, HrL, KpL, -ApL, ArL, z, z, z, true);
  XYZ_Coord sL;
  float Hyp = -turnRL;
  for (int i = 0; i < 3; i++)
  {
    sL = mf2b(Hyp, -HpL, HrL, KpL, -ApL, ArL, z, z, z, true);
    XYZ_Coord e((tL.x - sL.x), (tL.y - sL.y), (tL.z - sL.z));
    Hpr hpr = hipAngles(Hyp, -HpL, HrL, KpL, -ApL, ArL, z, z, z, e, true);
    HpL -= hpr.Hp;
    HrL += hpr.Hr;
  }
  // ApL and ArL to make sure LEFT foot is parallel to ground
  XYZ_Coord up = mf2b(Hyp, -HpL, HrL, KpL, -ApL, ArL, 1.0f, 0.0f, 0.0f, true);
  XYZ_Coord ur = mf2b(Hyp, -HpL, HrL, KpL, -ApL, ArL, 0.0f, 1.0f, 0.0f, true);
//  std::cout<<"apl1---->"<<RAD2DEG(ApL);
  ApL = ApL + asin(sL.z - up.z);
  ArL = ArL + asin(sL.z - ur.z);
//	std::cout<<"apl2---->"<<RAD2DEG(ApL)<<std::endl;
  // 9.5 Adjust HpR, HrR, ApR, ArR (RIGHT) based on Hyp turn to keep ankle in situ
  // Map to LEFT - we reuse the left foot IK because of symmetry right foot
  float Hr = -HrR;
  float Ar = -ArR;
  // Target foot origin in body coords
  XYZ_Coord t = mf2b(z, -HpR, Hr, KpR, -ApR, Ar, z, z, z, false);
  XYZ_Coord s;
  Hyp = -turnRL;
  for (int i = 0; i < 3; i++)
  {
    s = mf2b(Hyp, -HpR, Hr, KpR, -ApR, Ar, z, z, z, false);
    XYZ_Coord e((t.x - s.x), (t.y - s.y), (t.z - s.z));
    Hpr hpr = hipAngles(Hyp, -HpR, Hr, KpR, -ApR, Ar, z, z, z, e, false);
    HpR -= hpr.Hp;
    Hr += hpr.Hr;
  }
  // 9.6 Ap and Ar to make sure foot is parallel to ground
  XYZ_Coord u1 = mf2b(Hyp, -HpR, Hr, KpR, -ApR, Ar, 1.0f, 0.0f, 0.0f, false);
  XYZ_Coord u2 = mf2b(Hyp, -HpR, Hr, KpR, -ApR, Ar, 0.0f, 1.0f, 0.0f, false);
  ApR = ApR + asin(s.z - u1.z);
  Ar = Ar + asin(s.z - u2.z);
  // map back from left foot to right foot
  HrR = -Hr;
  ArR = -Ar;

  // 10. Set joint values and stiffness
  JointValues j = sensors.joints;
  for (uint8_t i = 0; i < Joints::numOfJoints; ++i)
    j.stiffnesses[i] = stiffness;

  // 10.1 Arms
  j.angles[Joints::lShoulderPitch] = DEG2RAD(90) + shoulderPitchL;
  j.angles[Joints::lShoulderRoll] = DEG2RAD(7) + shoulderRollL;
  j.angles[Joints::lElbowYaw] = DEG2RAD(0);  //DEG2RAD(-90); //swing bent arms
  j.angles[Joints::lElbowRoll] = DEG2RAD(0);  //DEG2RAD(-30)+shoulderPitchL;  //swing bent arms
  j.angles[Joints::lWristYaw] = DEG2RAD(0);
  j.angles[Joints::rShoulderPitch] = DEG2RAD(90) + shoulderPitchR;// * (1.f + (float)(active.forward)/(1000*MAX_FORWARD)); // yongqi: *1.5 to fix the problem that robot can't walk straight
  j.angles[Joints::rShoulderRoll] = DEG2RAD(-7) - shoulderRollR;
  j.angles[Joints::rElbowYaw] = DEG2RAD(0);  //DEG2RAD(90);  //swing bent arms
  j.angles[Joints::rElbowRoll] = DEG2RAD(0);  //DEG2RAD(30)-shoulderPitchR; //swing bent arms
  j.angles[Joints::rWristYaw] = DEG2RAD(0);

  float armStiffness = 0.1f;
  j.stiffnesses[Joints::lShoulderPitch] = armStiffness;
  j.stiffnesses[Joints::lShoulderRoll] = armStiffness;
  j.stiffnesses[Joints::lElbowYaw] = armStiffness;
  j.stiffnesses[Joints::lElbowRoll] = armStiffness;
  j.stiffnesses[Joints::lWristYaw] = armStiffness;
  j.stiffnesses[Joints::rShoulderPitch] = armStiffness;
  j.stiffnesses[Joints::rShoulderRoll] = armStiffness;
  j.stiffnesses[Joints::rElbowYaw] = armStiffness;
  j.stiffnesses[Joints::rElbowRoll] = armStiffness;
  j.stiffnesses[Joints::rWristYaw] = armStiffness;

  // 10.2 Turn
  j.angles[Joints::lHipYawPitch] = -turnRL;

  // 10.3 Sagittal Joints
  j.angles[Joints::lHipPitch] = -HpL;
  j.angles[Joints::rHipPitch] = -HpR;
  j.angles[Joints::lKneePitch] = KpL;
  j.angles[Joints::rKneePitch] = KpR;
  // Only activate balance control if foot is on the ground
  j.angles[Joints::lAnklePitch] = -ApL;
  j.angles[Joints::rAnklePitch] = -ApR;
//#ifdef TARGET_ROBOT
  if (walkOption == WALK and nextFootSwitchT > 0)
  {
    if (bodyModel.isLeftPhase)
      j.angles[Joints::rAnklePitch] += balanceAdjustment;
    else
      j.angles[Joints::lAnklePitch] += balanceAdjustment;
  }
  else if (walkOption == KICK)
  {
    if (bodyModel.isLeftPhase)
      j.angles[Joints::lAnklePitch] += balanceAdjustment;
    else
      j.angles[Joints::rAnklePitch] += balanceAdjustment;
  }
  else
  {
    j.angles[Joints::rAnklePitch] += balanceAdjustment;
    j.angles[Joints::lAnklePitch] += balanceAdjustment;
  }
//#endif

  // 10.4 Coronal Joints
  j.angles[Joints::lHipRoll] = HrL;
  j.angles[Joints::rHipRoll] = HrR;
  j.angles[Joints::lAnkleRoll] = ArL;
  j.angles[Joints::rAnkleRoll] = ArR;

//  j.angles[Joints::lHipYawPitch] += -3_deg;

  // Add in joint adjustments for kicks
  if (walkOption == KICK)
  {
    addKickJoints(j);
    // Add in some coronal balancing
    if (bodyModel.isLeftPhase) {
      j.angles[Joints::rAnkleRoll] += coronalBalanceAdjustment;
    }
    else {
      j.angles[Joints::lAnkleRoll] += coronalBalanceAdjustment;
    }
  }
//  std::cout<<"hippitch---->"<<RAD2DEG(j.angles[Joints::lHipPitch])<<", kneepitch----->"<<RAD2DEG(j.angles[Joints::lKneePitch])<<", anklepitch----->"<<RAD2DEG(j.angles[Joints::lAnklePitch])<<", ApL---->"<<RAD2DEG(ApL)<<std::endl;
  /*
   //quick check if position went out of joint space
   for(int i = 0; i < cJoints::numOfJoints; i++){
   checkNaN(j.angles[i],  cJoints::fliteJointNames[i]);
   }
   */
  return j;
}

void WalkingEngine::generateOutput(WalkingEngineOutput& walkingEngineOutput)
{
  walkingEngineOutput.speed.translation = Vector2f(forward, left);
  walkingEngineOutput.speed.rotation = lastTurn;
  Pose3f footLeft = theRobotModel.limbs[Limbs::footLeft].translated(0.f, 0.f, -theRobotDimensions.footHeight);
  Pose3f footRight = theRobotModel.limbs[Limbs::footRight].translated(0.f, 0.f, -theRobotDimensions.footHeight);
  Vector3f odometryOrigin = (footLeft.translation + footRight.translation) * 0.5f;
  if(lastOdometryOrigin.z() != 0.f)
  {
    Pose3f& footSupport = bodyModel.isLeftPhase ? footLeft : footRight;
    Pose3f& lastFootSupport = bodyModel.isLeftPhase ? lastFootLeft : lastFootRight;
    Pose3f odometryOffset3DinP = (Pose3f(-odometryOrigin).conc(footSupport).conc(lastFootSupport.inverse()).conc(lastOdometryOrigin)).inverse();
    Pose3f odometryOffset3D = Pose3f(theTorsoMatrix).conc(odometryOffset3DinP).conc(theTorsoMatrix.inverse());
    odometryOffset.rotation = odometryOffset3D.rotation.getZAngle();
    odometryOffset.translation.x() = -odometryOffset3D.translation.x();
    odometryOffset.translation.y() = -odometryOffset3D.translation.y();
  }
  else
    odometryOffset = Pose2f();
  lastFootLeft = footLeft;
  lastFootRight = footRight;
  lastOdometryOrigin = odometryOrigin;
  walkingEngineOutput.odometryOffset = odometryOffset;

  walkingEngineOutput.upcomingOdometryOffset = Pose2f(odometry.turn, odometry.forward, odometry.left);
  static Vector2f odometer(0,0);
  odometer.x() += odometry.forward;
  odometer.y() += odometry.left;

  walkingEngineOutput.upcomingOdometryOffsetValid = true;
  walkingEngineOutput.isLeavingPossible = active.actionType != ActionCommand::Body::WALK;
  walkingEngineOutput.positionInWalkCycle = 0.f;
  walkingEngineOutput.standing = active.actionType != ActionCommand::Body::WALK;
  walkingEngineOutput.instability = 0.f;
  walkingEngineOutput.executedWalk = theMotionRequest.walkRequest;
  // walkingEngineOutput.executedWalk.kickType = WalkRequest::none; //kickPlayer.isActive() ? kickPlayer.getType() : WalkRequest::none;
  (JointRequest&)walkingEngineOutput = jointRequest;
  WalkingEngineOutput::PhaseType phase;
  if (active.actionType == ActionCommand::Body::STAND)
    phase = WalkingEngineOutput::standPhase;
  else if (bodyModel.isLeftPhase)
    phase = WalkingEngineOutput::rightSupportPhase;
  else
    phase = WalkingEngineOutput::leftSupportPhase;
  walkingEngineOutput.walkPhase = phase;

  walkingEngineOutput.actionFlag = actionFlag;
  walkingEngineOutput.kickFinish = kickFinish;
}

void WalkingEngine::generateDummyOutput(WalkingEngineOutput& walkingEngineOutput)
{
  walkingEngineOutput.standing = false;
  walkingEngineOutput.speed = Pose2f();
  walkingEngineOutput.odometryOffset = Pose2f();
  walkingEngineOutput.upcomingOdometryOffset = Pose2f();
  walkingEngineOutput.upcomingOdometryOffsetValid = true;
  walkingEngineOutput.isLeavingPossible = true;
  walkingEngineOutput.positionInWalkCycle = 0.f;
  walkingEngineOutput.instability = 0.f;
  walkingEngineOutput.executedWalk = WalkRequest();
  walkingEngineOutput.walkPhase = WalkingEngineOutput::PhaseType::standPhase;
  // leaving joint data untouched
}



Odometry WalkingEngine::updateOdometry(bool isLeftSwingFoot)
{
  // Work out incremental forward, left, and turn values for next time step
  float height = hiph - ankle;
  float turnOdo = -(turnRL - prevTurn);
  float leftOdo = (height * tan(leftR) - prevLeftR);
  float forwardOdo = (forwardR - prevForwardR);
  if (!isLeftSwingFoot)
  {
    turnOdo *= -1;
    leftOdo = (height * tan(leftL) - prevLeftL);
    forwardOdo = (forwardL - prevForwardL);
  }
  forwardOdo *= MM_PER_M;
  leftOdo *= MM_PER_M;
  //Calibrate odometry to match the actual speed
  forwardOdo *= 1;
  leftOdo *= 1.23;
  turnOdo *= -.58;
  //cout << forwardOdo <<" "<< leftOdo <<" "<< turnOdo << endl;

  // backup odometry values
  prevTurn = turnRL;
  prevLeftL = height * tan(leftL);
  prevLeftR = height * tan(leftR);
  prevForwardL = forwardL;
  prevForwardR = forwardR;
  return Odometry(forwardOdo, leftOdo, turnOdo);
}

float WalkingEngine::leftAngle()
{
  float left_at_t = left*parabolicStep(t,nextFootSwitchT,0.0);
  float height = hiph-ankle;
  return atan(left_at_t/height);
}

// functions out of class
float WalkingEngine::linearStep(float time, float period)
{
  if (time <= 0)
    return 0;
  if (time >= period)
    return 1;
  return time / period;
}

float WalkingEngine::parabolicReturn(float f)
{         //normalised [0,1] up and down
  double x = 0;
  double y = 0;
  if (f < 0.25f)
  {
    y = 8 * f * f;
  }
  if (f >= 0.25f && f < 0.5f)
  {
    x = 0.5f - f;
    y = 8 * x * x;
    y = 1.0f - y;
  }
  if (f >= 0.5f && f < 0.75f)
  {
    x = f - 0.5f;
    y = 8 * x * x;
    y = 1.0f - y;
  }
  if (f >= 0.75f && f <= 1.0f)
  {
    x = 1.0f - f;
    y = 8 * x * x;
  }
  return y;
}

float WalkingEngine::parabolicStep(float time, float period, float deadTimeFraction)
{  //normalised [0,1] step up
  float deadTime = period * deadTimeFraction / 2;
  if (time < deadTime + dt / 2)
    return 0;
  if (time > period - deadTime - dt / 2)
    return 1;
  float timeFraction = (time - deadTime) / (period - 2 * deadTime);
  if (time < period / 2)
    return 2.0 * timeFraction * timeFraction;
  return 4 * timeFraction - 2 * timeFraction * timeFraction - 1;
}

float WalkingEngine::interpolateSmooth(float start, float end, float tCurrent, float tEnd) {
   return start + (end - start) * (1 + cos(M_PI * tCurrent / tEnd - M_PI)) / 2;
}

// 支撑脚6+1个关节角+摆动脚坐标位置------DH链
XYZ_Coord WalkingEngine::mf2b(float Hyp, float Hp, float Hr,
                                  float Kp,  float Ap, float Ar,
                                  float xf, float yf, float zf, bool left) {
// MFOOT2BODY Transform coords from foot to body.
// This code originates from 2010 using symbolic equations in Matlab to perform the coordinate transforms - see team report (BH)
// In future this approach to IK for the Nao should be reworked in closed form, significantly reducing the size of the code the
// the computational complexity (BH)
   XYZ_Coord result;
   float pi = M_PI;
   float tibia        = this->tibia*1000;
   float thigh        = left ? this->thighL*1000 : this->thighR*1000;
   float k  = sqrt(2.0);
   float c1 = cos(Ap);
   float c2 = cos(Hr + pi/4.0);
   float c3 = cos(Hyp - pi/2.0);
   float c4 = cos(Hp);
   float c5 = cos(Kp);
   float c6 = cos(Ar - pi/2.0);
   float s1 = sin(Kp);
   float s2 = sin(Hp);
   float s3 = sin(Hyp - 1.0/2.0*pi);
   float s4 = sin(Hr + 1.0/4.0*pi);
   float s5 = sin(Ap);
   float s6 = sin(Ar - 1.0/2.0*pi);
   result.x = thigh*(s2*s3 - c2*c3*c4) + tibia*(s1*(c4*s3 + c2*c3*s2) +
       c5*(s2*s3 - c2*c3*c4)) - yf*(c6*(c1*(s1*(c4*s3 + c2*c3*s2) +
       c5*(s2*s3 - c2*c3*c4)) - s5*(s1*(s2*s3 - c2*c3*c4) - c5*(c4*s3 +
       c2*c3*s2))) + c3*s4*s6) + zf*(s6*(c1*(s1*(c4*s3 + c2*c3*s2) +
       c5*(s2*s3 - c2*c3*c4)) - s5*(s1*(s2*s3 - c2*c3*c4) - c5*(c4*s3 +
       c2*c3*s2))) - c3*c6*s4) + xf*(c1*(s1*(s2*s3 - c2*c3*c4) -
       c5*(c4*s3 + c2*c3*s2)) + s5*(s1*(c4*s3 + c2*c3*s2) +
       c5*(s2*s3 - c2*c3*c4)));
   result.y = xf*(c1*(c5*(s2*((k*s4)/2.0f + (c2*k*s3)/2.0f) -
       (c3*c4*k)/2.0f) + s1*(c4*((k*s4)/2.0f + (c2*k*s3)/2.0f) +
       (c3*k*s2)/2.0f)) + s5*(c5*(c4*((k*s4)/2.0f + (c2*k*s3)/2.0f) +
       (c3*k*s2)/2.0f) - s1*(s2*((k*s4)/2.0f + (c2*k*s3)/2.0f) -
       (c3*c4*k)/2.0f))) + tibia*(c5*(c4*((k*s4)/2.0f +
       (c2*k*s3)/2.0f) + (c3*k*s2)/2.0f) - s1*(s2*((k*s4)/2.0f +
       (c2*k*s3)/2.0f) - (c3*c4*k)/2.0f)) + thigh*(c4*((k*s4)/2.0f +
       (c2*k*s3)/2.0f) + (c3*k*s2)/2.0f) - yf*(s6*((c2*k)/2.0f -
       (k*s3*s4)/2.0f) + c6*(c1*(c5*(c4*((k*s4)/2.0f + (c2*k*s3)/2.0f) +
       (c3*k*s2)/2.0f) - s1*(s2*((k*s4)/2.0f + (c2*k*s3)/2.0f) -
       (c3*c4*k)/2.0f)) - s5*(c5*(s2*((k*s4)/2.0f + (c2*k*s3)/2.0f) -
       (c3*c4*k)/2.0f) + s1*(c4*((k*s4)/2.0f + (c2*k*s3)/2.0f) +
       (c3*k*s2)/2.0f)))) - zf*(c6*((c2*k)/2.0f - (k*s3*s4)/2.0f) -
       s6*(c1*(c5*(c4*((k*s4)/2.0f + (c2*k*s3)/2.0f) + (c3*k*s2)/2.0f) -
       s1*(s2*((k*s4)/2.0f + (c2*k*s3)/2.0f) - (c3*c4*k)/2.0f)) -
       s5*(c5*(s2*((k*s4)/2.0f + (c2*k*s3)/2.0f) - (c3*c4*k)/2.0f) +
       s1*(c4*((k*s4)/2.0f + (c2*k*s3)/2.0f) + (c3*k*s2)/2.0f))));
   result.z = yf*(s6*((c2*k)/2.0f + (k*s3*s4)/2.0f) +
       c6*(c1*(c5*(c4*((k*s4)/2.0f - (c2*k*s3)/2.0f) - (c3*k*s2)/2.0f) -
       s1*(s2*((k*s4)/2.0f - (c2*k*s3)/2.0f) + (c3*c4*k)/2.0f)) -
       s5*(c5*(s2*((k*s4)/2.0f - (c2*k*s3)/2.0f) + (c3*c4*k)/2.0f) +
       s1*(c4*((k*s4)/2.0f - (c2*k*s3)/2.0f) - (c3*k*s2)/2.0f)))) -
       tibia*(c5*(c4*((k*s4)/2.0f - (c2*k*s3)/2.0f) - (c3*k*s2)/2.0f) -
       s1*(s2*((k*s4)/2.0f - (c2*k*s3)/2.0f) + (c3*c4*k)/2.0f)) -
       thigh*(c4*((k*s4)/2.0f - (c2*k*s3)/2.0f) - (c3*k*s2)/2.0f) -
       xf*(c1*(c5*(s2*((k*s4)/2.0f - (c2*k*s3)/2.0f) + (c3*c4*k)/2.0f) +
       s1*(c4*((k*s4)/2.0f - (c2*k*s3)/2.0f) - (c3*k*s2)/2.0f)) +
       s5*(c5*(c4*((k*s4)/2.0f - (c2*k*s3)/2.0f) - (c3*k*s2)/2.0f) -
       s1*(s2*((k*s4)/2.0f - (c2*k*s3)/2.0f) + (c3*c4*k)/2.0f))) +
       zf*(c6*((c2*k)/2.0f + (k*s3*s4)/2.0f) - s6*(c1*(c5*(c4*((k*s4)/2.0f -
       (c2*k*s3)/2.0f) - (c3*k*s2)/2.0f) - s1*(s2*((k*s4)/2.0f -
       (c2*k*s3)/2.0f) + (c3*c4*k)/2.0f)) - s5*(c5*(s2*((k*s4)/2.0f -
       (c2*k*s3)/2.0f) + (c3*c4*k)/2.0f) + s1*(c4*((k*s4)/2.0f -
       (c2*k*s3)/2.0f) - (c3*k*s2)/2.0f))));
   return result;
}

WalkingEngine::Hpr WalkingEngine::hipAngles(float Hyp, float Hp,
                                  float Hr, float Kp,  float Ap, float Ar,
                                  float xf, float yf, float zf, XYZ_Coord e, bool left) {
// Code from 2010 to perform interative Inverse Kinematics.
// Symbolic equations generated in Matlab - see 2010 team report for details and reference
   Hpr result;
   float pi = M_PI;
   float tibia        = this->tibia*1000;
   float thigh        = left ? this->thighL*1000 : this->thighR*1000;
   float k  = sqrt(2.0);
   float c1 = cos(Ap);
   float c2 = cos(Hr + pi/4.0);
   float c3 = cos(Hyp - pi/2.0);
   float c4 = cos(Hp);
   float c5 = cos(Kp);
   float c6 = cos(Ar - pi/2.0);
   float s1 = sin(Kp);
   float s2 = sin(Hp);
   float s3 = sin(Hyp - 1.0/2.0*pi);
   float s4 = sin(Hr + 1.0/4.0*pi);
   float s5 = sin(Ap);
   float s6 = sin(Ar - 1.0/2.0*pi);
   float j11 = thigh*(c4*s3 + c2*c3*s2) - tibia*(s1*(s2*s3 -
       c2*c3*c4) - c5*(c4*s3 + c2*c3*s2)) + xf*(c1*(s1*(c4*s3 +
       c2*c3*s2) + c5*(s2*s3 - c2*c3*c4)) - s5*(s1*(s2*s3 -
       c2*c3*c4) - c5*(c4*s3 + c2*c3*s2))) + c6*yf*(c1*(s1*(s2*s3 -
       c2*c3*c4) - c5*(c4*s3 + c2*c3*s2)) + s5*(s1*(c4*s3 +
       c2*c3*s2) + c5*(s2*s3 - c2*c3*c4))) - s6*zf*(c1*(s1*(s2*s3 -
       c2*c3*c4) - c5*(c4*s3 + c2*c3*s2)) + s5*(s1*(c4*s3 +
       c2*c3*s2) + c5*(s2*s3 - c2*c3*c4)));
   float j12 = yf*(c6*(c1*(c3*s1*s2*s4 - c3*c4*c5*s4) +
       s5*(c3*c4*s1*s4 + c3*c5*s2*s4)) - c2*c3*s6) -
       tibia*(c3*s1*s2*s4 - c3*c4*c5*s4) - zf*(s6*(c1*(c3*s1*s2*s4 -
       c3*c4*c5*s4) + s5*(c3*c4*s1*s4 + c3*c5*s2*s4)) + c2*c3*c6) +
       xf*(c1*(c3*c4*s1*s4 + c3*c5*s2*s4) - s5*(c3*s1*s2*s4 -
       c3*c4*c5*s4)) + c3*c4*s4*thigh;
   float j21 = xf*(c1*(c5*(c4*((k*s4)/2.0f + (c2*k*s3)/2.0f) +
       (c3*k*s2)/2.0f) - s1*(s2*((k*s4)/2.0f + (c2*k*s3)/2.0f) -
       (c3*c4*k)/2.0f)) -
       s5*(c5*(s2*((k*s4)/2.0f + (c2*k*s3)/2.0f) - (c3*c4*k)/2.0f) +
       s1*(c4*((k*s4)/2.0f + (c2*k*s3)/2.0f) + (c3*k*s2)/2.0f))) -
       tibia*(c5*(s2*((k*s4)/2.0f + (c2*k*s3)/2.0f) - (c3*c4*k)/2.0f) +
       s1*(c4*((k*s4)/2.0f + (c2*k*s3)/2.0f) + (c3*k*s2)/2.0f)) -
       thigh*(s2*((k*s4)/2.0f + (c2*k*s3)/2.0f) - (c3*c4*k)/2.0f) +
       c6*yf*(c1*(c5*(s2*((k*s4)/2.0f + (c2*k*s3)/2.0f) - (c3*c4*k)/2.0f) +
       s1*(c4*((k*s4)/2.0f + (c2*k*s3)/2.0f) + (c3*k*s2)/2.0f)) +
       s5*(c5*(c4*((k*s4)/2.0f + (c2*k*s3)/2.0f) + (c3*k*s2)/2.0f) -
       s1*(s2*((k*s4)/2.0f + (c2*k*s3)/2.0f) - (c3*c4*k)/2.0f))) -
       s6*zf*(c1*(c5*(s2*((k*s4)/2.0f + (c2*k*s3)/2.0f) - (c3*c4*k)/2.0f) +
       s1*(c4*((k*s4)/2.0f + (c2*k*s3)/2.0f) + (c3*k*s2)/2.0f)) +
       s5*(c5*(c4*((k*s4)/2.0f + (c2*k*s3)/2.0f) + (c3*k*s2)/2.0f) -
       s1*(s2*((k*s4)/2.0f + (c2*k*s3)/2.0f) - (c3*c4*k)/2.0f)));
   float j22 = tibia*(c4*c5*((c2*k)/2.0f - (k*s3*s4)/2.0f) -
       s1*s2*((c2*k)/2.0f - (k*s3*s4)/2.0f)) + xf*(c1*(c4*s1*((c2*k)/2.0f -
       (k*s3*s4)/2.0f) + c5*s2*((c2*k)/2.0f - (k*s3*s4)/2.0f)) +
       s5*(c4*c5*((c2*k)/2.0f - (k*s3*s4)/2.0f) - s1*s2*((c2*k)/2.0f -
       (k*s3*s4)/2.0f))) + yf*(s6*((k*s4)/2.0f + (c2*k*s3)/2.0f) -
       c6*(c1*(c4*c5*((c2*k)/2.0f - (k*s3*s4)/2.0f) - s1*s2*((c2*k)/2.0f -
       (k*s3*s4)/2.0f)) - s5*(c4*s1*((c2*k)/2.0f - (k*s3*s4)/2.0f) +
       c5*s2*((c2*k)/2.0f - (k*s3*s4)/2.0f)))) + zf*(c6*((k*s4)/2.0f +
       (c2*k*s3)/2.0f) + s6*(c1*(c4*c5*((c2*k)/2.0f - (k*s3*s4)/2.0f) -
       s1*s2*((c2*k)/2.0f - (k*s3*s4)/2.0f)) - s5*(c4*s1*((c2*k)/2.0f -
       (k*s3*s4)/2.0f) + c5*s2*((c2*k)/2.0f - (k*s3*s4)/2.0f)))) +
       c4*thigh*((c2*k)/2.0f - (k*s3*s4)/2.0f);
   float j31 = tibia*(c5*(s2*((k*s4)/2.0f - (c2*k*s3)/2.0f) +
       (c3*c4*k)/2.0f) + s1*(c4*((k*s4)/2.0f - (c2*k*s3)/2.0f) -
       (c3*k*s2)/2.0f)) -
       xf*(c1*(c5*(c4*((k*s4)/2.0f - (c2*k*s3)/2.0f) - (c3*k*s2)/2.0f) -
       s1*(s2*((k*s4)/2.0f - (c2*k*s3)/2.0f) + (c3*c4*k)/2.0f)) -
       s5*(c5*(s2*((k*s4)/2.0f - (c2*k*s3)/2.0f) + (c3*c4*k)/2.0f) +
       s1*(c4*((k*s4)/2.0f - (c2*k*s3)/2.0f) - (c3*k*s2)/2.0f))) +
       thigh*(s2*((k*s4)/2.0f - (c2*k*s3)/2.0f) + (c3*c4*k)/2.0f) -
       c6*yf*(c1*(c5*(s2*((k*s4)/2.0f - (c2*k*s3)/2.0f) + (c3*c4*k)/2.0f) +
       s1*(c4*((k*s4)/2.0f - (c2*k*s3)/2.0f) - (c3*k*s2)/2.0f)) +
       s5*(c5*(c4*((k*s4)/2.0f - (c2*k*s3)/2.0f) - (c3*k*s2)/2.0f) -
       s1*(s2*((k*s4)/2.0f - (c2*k*s3)/2.0f) + (c3*c4*k)/2.0f))) +
       s6*zf*(c1*(c5*(s2*((k*s4)/2.0f - (c2*k*s3)/2.0f) + (c3*c4*k)/2.0f) +
       s1*(c4*((k*s4)/2.0f - (c2*k*s3)/2.0f) - (c3*k*s2)/2.0f)) +
       s5*(c5*(c4*((k*s4)/2.0f - (c2*k*s3)/2.0f) - (c3*k*s2)/2.0f) -
       s1*(s2*((k*s4)/2.0f - (c2*k*s3)/2.0f) + (c3*c4*k)/2.0f)));
   float j32 = -tibia*(c4*c5*((c2*k)/2.0f + (k*s3*s4)/2.0f) -
       s1*s2*((c2*k)/2.0f + (k*s3*s4)/2.0f)) - xf*(c1*(c4*s1*((c2*k)/2.0f +
       (k*s3*s4)/2.0f) + c5*s2*((c2*k)/2.0f + (k*s3*s4)/2.0f)) +
       s5*(c4*c5*((c2*k)/2.0f + (k*s3*s4)/2.0f) - s1*s2*((c2*k)/2.0f +
       (k*s3*s4)/2.0f))) - yf*(s6*((k*s4)/2.0f - (c2*k*s3)/2.0f) -
       c6*(c1*(c4*c5*((c2*k)/2.0f + (k*s3*s4)/2.0f) - s1*s2*((c2*k)/2.0f +
       (k*s3*s4)/2.0f)) - s5*(c4*s1*((c2*k)/2.0f + (k*s3*s4)/2.0f) +
       c5*s2*((c2*k)/2.0f + (k*s3*s4)/2.0f)))) - zf*(c6*((k*s4)/2.0f -
       (c2*k*s3)/2.0f) + s6*(c1*(c4*c5*((c2*k)/2.0f + (k*s3*s4)/2.0f) -
       s1*s2*((c2*k)/2.0f + (k*s3*s4)/2.0f)) - s5*(c4*s1*((c2*k)/2.0f +
       (k*s3*s4)/2.0f) + c5*s2*((c2*k)/2.0f + (k*s3*s4)/2.0f)))) -
       c4*thigh*((c2*k)/2.0f + (k*s3*s4)/2.0f);
   float xbe = e.x;
   float ybe = e.y;
   float zbe = e.z;
   float lambda = 0.4f;
   float la2 = lambda*lambda;
   float la4 = la2*la2;
   float j322 = j32*j32;
   float j222 = j22*j22;
   float j122 = j12*j12;
   float j212 = j21*j21;
   float j112 = j11*j11;
   float j312 = j31*j31;
   float sigma = 1.0f/(la4 + j112*j222 + j122*j212 + j112*j322 + j122*j312 +
   j212*j322 + j222*j312 + j112*la2 + j122*la2 + j212*la2 + j222*la2 +
   j312*la2 + j322*la2 - 2.0f*j11*j12*j21*j22 - 2.0f*j11*j12*j31*j32 -
   2.0f*j21*j22*j31*j32);
   result.Hp = sigma*xbe*(j11*j222 + j11*j322 + j11*la2 - j12*j21*j22 -
   j12*j31*j32) + sigma*ybe*(j122*j21 + j21*j322 + j21*la2 - j11*j12*j22 -
   j22*j31*j32) + sigma*zbe*(j122*j31 + j222*j31 + j31*la2 - j11*j12*j32 -
   j21*j22*j32);
   result.Hr =  sigma*xbe*(j12*j212 + j12*j312 + j12*la2 - j11*j21*j22 -
   j11*j31*j32) + sigma*ybe*(j112*j22 + j22*j312 + j22*la2 - j11*j12*j21 -
   j21*j31*j32) + sigma*zbe*(j112*j32 + j212*j32 + j32*la2 - j11*j12*j31 -
   j21*j22*j31);
   return result;
}

void WalkingEngine::walkKick(float ballX, BodyModel &bodyModel) {
  // plan forward
//  cout<<"walkKick\n";
  float delta = 0.1f; // meters 0.05
  float ballDistX = ballX/MM_PER_M;
  if(ActionCommand::Body::LEFT) { // left foot walk-kick
    //cout <<"forward-0 "<< ballX <<" "<< forward << endl;
    if(bodyModel.isLeftPhase and (ballDistX-delta+forwardL0)<forward) {
      forward = (ballDistX-delta-forwardL0)/2;
      //cout <<"forward-1 "<< ballX <<" "<< forward <<" "<< forwardL0 <<" "<< delta << endl;
    }
    if(!bodyModel.isLeftPhase and (ballDistX-delta-forwardR0)<forward/2) {
      forward = ballDistX-delta-forwardR0;
      //cout <<"forward-2 "<< ballX <<" "<< forward <<" "<< forwardR0 <<" "<< delta << endl;
    }
    if(bodyModel.isLeftPhase and (ballDistX-delta+forwardL0)<delta)  forward = abs(power)*MAX_FORWARD;
      if(!bodyModel.isLeftPhase and (ballDistX-delta-forwardR0)<delta) forward = abs(power)*MAX_FORWARD;
    //cout <<"forward-3 "<< ballX <<" "<< forward << endl;
  }
  if(ActionCommand::Body::RIGHT) { // left foot walk-kick
    //cout <<"forward-0 "<< ballX <<" "<< forward << endl;
    if(!bodyModel.isLeftPhase and (ballDistX-delta+forwardR0)<forward) {
      forward = (ballDistX-delta-forwardR0)/2;
      //cout <<"forward-1 "<< ballX <<" "<< forward <<" "<< forwardL0 <<" "<< delta << endl;
    }
    if(bodyModel.isLeftPhase and (ballDistX-delta-forwardL0)<forward/2) {
      forward = ballDistX-delta-forwardL0;
      //cout <<"forward-2 "<< ballX <<" "<< forward <<" "<< forwardR0 <<" "<< delta << endl;
    }
    if(!bodyModel.isLeftPhase and (ballDistX-delta+forwardR0)<delta)  forward = abs(power)*MAX_FORWARD;
      if(bodyModel.isLeftPhase and (ballDistX-delta-forwardL0)<delta) forward = abs(power)*MAX_FORWARD;
    //cout <<"forward-3 "<< ballX <<" "<< forward << endl;
  }
    if(forward>MAX_FORWARD) forward = MAX_FORWARD; // in case things go wrong
    if(forward>MAX_FORWARD) forward = MAX_FORWARD; // in case things go wrong
  //cout <<"forward-4 "<< ballX <<" "<< forward << endl;
}

void WalkingEngine::prepKick(bool isLeft, BodyModel &bodyModel) {
   t = 0;
   walkOption = KICK;
   turnAngle = 0;
   kickT = 0;
   flashKickT = 0;
   if (isLeft) {
      lastKickForward = forwardL;
      lastKneePitch = kneePitchL;
      lastSide = leftL;
      bodyModel.setIsLeftPhase(true);
      forwardR = 0;
      foothR = 0;
   } else {
      lastKickForward = forwardR;
      lastKneePitch = kneePitchR;
      lastSide = -leftR;
      bodyModel.setIsLeftPhase(false);
      forwardL = 0;
      foothL = 0;
   }
}

// nicer for the motors
bool WalkingEngine::canAbbortKick() {
    float totalShift = SHIFT_PERIOD / 4;
    float halfShift = totalShift / 2;
    float pauseTime = (BACK_PHASE - totalShift) / 2;
    return false;
    // return kickT - pauseTime < halfShift
    //         || (kickT >= totalShift + pauseTime && kickT < BACK_PHASE);
}

void WalkingEngine::addKickJoints(JointValues &j){
   j.angles[Joints::lKneePitch] += kneePitchL;
   j.angles[Joints::lAnklePitch] += anklePitchL + shoulderRollL;
   j.angles[Joints::lShoulderPitch] -= shoulderPitchAdjustL;
   j.angles[Joints::lShoulderRoll] += DEG2RAD(5);
   j.angles[Joints::lHipPitch] += hipPitchL;
   j.angles[Joints::lAnkleRoll] += lankleRollAdjust;

   j.angles[Joints::rKneePitch] += kneePitchR;
   j.angles[Joints::rAnklePitch] += anklePitchR + shoulderRollR;
   j.angles[Joints::rShoulderPitch] -= shoulderPitchAdjustR;
   j.angles[Joints::rShoulderRoll] -= DEG2RAD(5);
   j.angles[Joints::rHipPitch] += hipPitchR;
   j.angles[Joints::rAnkleRoll] += rankleRollAdjust;
}

void WalkingEngine::makeFlashKickJoints(float flashKickLean, float flashKickStepH, float flashShiftPeriod, float &footh, float &forwardDist, float &side,float &kneePitch, float &shoulderRoll, float &shoulderPitchAdjust, float &anklePitch, float &hipPitchAdjust, float &ballY, ActionCommand::Body* request)
{
  cout << "hipPitchAdjust---> " << RAD2DEG(hipPitchAdjust) << endl;
  flashKickT += dt;
  float hipPitchAmp = DEG2RAD(-20);
  float anklePitchAmp = DEG2RAD(20);
  float shoulderPitchAmp = DEG2RAD(-20);
  /*
  FLASH_BACK_PHASE = 0.4
  flashShiftPeriod = 1.2
  flashKickPhase = 0.3
   */
  if (flashKickT < FLASH_BACK_PHASE)
  {
    float flashTotalShift = flashShiftPeriod / 4;
    float flashHalfShift = flashTotalShift / 2;
    float flashPauseTime = (FLASH_BACK_PHASE - flashTotalShift) / 2;
    if (flashKickT >= flashPauseTime)
    {
      float t2 = flashKickT - flashPauseTime;

        rock = DEG2RAD(flashKickLean) * parabolicStep(t2, flashTotalShift, 0);
    }
  }

  else if(flashKickT < (FLASH_BACK_PHASE + flashKickPhase))
  {
    float t1 = flashKickT - FLASH_BACK_PHASE;
		hipPitchAdjust = interpolateSmooth(0,hipPitchAmp,t1,flashKickPhase);
		anklePitch = interpolateSmooth(0, anklePitchAmp, t1,flashKickPhase);
    shoulderPitchAdjust = interpolateSmooth(0, shoulderPitchAmp, t1,flashKickPhase);
  }

  else
  {
    flashKickT = 0;
    kickFinish = true;
    isKicking = false;
    rock = 0;
    anklePitch = 0;
    hipPitchAdjust = 0;
    shoulderPitchAdjust = 0;
    walkOption= WALK;
    walkState = NOT_WALKING;
    request->actionType = ActionCommand::Body::WALK;
    lastFlashKickTime = 0;
  }

}


void WalkingEngine::makeForwardKickJoints(float kickLean, float kickStepH,
                                          float shiftPeriod, float &footh,
                                          float &forwardDist, float &side,
                                          float &kneePitch, float &shoulderRoll, float &shoulderPitchAdjust,
                                          float &anklePitch, float &ballY,
                                          ActionCommand::Body* request)
{

  kickT += dt;
  kickPhase = MAX_KICK_PHASE;
  float totalPhase = BACK_PHASE + kickPhase + THROUGH_PHASE + END_PHASE;

  // Update side position of ball as late as possible.
  if (request->misalignedKick)
  {
    dynamicSide = 0;  // This will try to kick the robot with the outside of the foot.
  }
  else if (kickT < BACK_PHASE * 0.8)
  {
    dynamicSide = fabs(ballY) - 40;  // offset to kick point on foot
  }
  // Max safe/useful value for lifting leg (without going past joint limits/balancing)
  float sideAmp = -MAX(0, MIN(50, dynamicSide)) / 200.0;
  float kickAmp = -0.07;  // how far forward the kick should reach
  float kickPower;
  kickPower = pow(power, 1.7);
  float kickBackAmp = -kickAmp * kickPower * 0.9;
  float shoulderRollAmp = -sideAmp / 2.5;  // how much arm should lift to make room for raised kicking leg

  float kneePitchAmp = DEG2RAD(35);
  float anklePitchAmp = DEG2RAD(35);
  float anklePitchRetracted = DEG2RAD(5);
  kickStepH += 0.01;

  if (lastTurn > 0)
  {
    shiftPeriod = 3.5;
  }

  // Shift weight over and swing foot back
  if (kickT < BACK_PHASE)
  {
    float totalShift = shiftPeriod / 4;
    float halfShift = totalShift / 2;
    float pauseTime = (BACK_PHASE - totalShift) / 2;
    if (t >= pauseTime)
    {
      float t2 = t - pauseTime;
      // We spend the first part shifting our weight over.
      if (t2 < totalShift)
      {
        rock = DEG2RAD(kickLean) * parabolicStep(t2, totalShift, 0);
        // If the robot had turned, bring the feet back together after some time for foot to lift off the ground
        if (lastTurn > 0)
        {
          if (t2 >= halfShift)
          {
            turnRL = lastTurn
                * (1 - parabolicStep(t2 - halfShift, halfShift, 0.0));
          }
          else
          {
            turnRL = lastTurn;
          }
          // Add in some extra lean while shifting weight along with turn
          rock += DEG2RAD(kickLean) / 3.5 * parabolicReturn(t2 / totalShift);
        }
      }
      else
      {
        turnRL = 0;
        lastTurn = 0;
      }

      if (t2 >= totalShift / 3)
      {
        float t3 = t2 - totalShift / 3;
        float endT = BACK_PHASE - totalShift / 3 - pauseTime;
        footh = kickStepH * parabolicStep(t3, endT, 0);
      }

      float kickT2 = kickT - pauseTime;
      // Once we're halfway through shifting our weight, start moving the foot back.
      if (kickT2 >= halfShift)
      {
        float shiftedKickT = kickT2 - halfShift;
        float endT = BACK_PHASE - halfShift - pauseTime;
        forwardDist = interpolateSmooth(0, kickBackAmp, shiftedKickT, endT);
        side = interpolateSmooth(0, sideAmp, shiftedKickT, endT);
        shoulderRoll = interpolateSmooth(0, shoulderRollAmp, shiftedKickT,
                                         endT);
        kneePitch = interpolateSmooth(0, kneePitchAmp * 0.9, shiftedKickT,
                                      endT);
        anklePitch = interpolateSmooth(0, anklePitchRetracted, shiftedKickT,
                                       endT);
        shoulderPitchAdjust = kneePitch;
      }
    }
    // Swing foot forward.
  }
  else if (kickT < (BACK_PHASE + kickPhase))
  {
    forwardDist = interpolateSmooth(kickBackAmp, kickAmp, kickT - BACK_PHASE,
                                    kickPhase);
    side = sideAmp;
    shoulderRoll = shoulderRollAmp;
    kneePitch = interpolateSmooth(kneePitchAmp * 0.9, -kneePitchAmp,
                                  kickT - BACK_PHASE, kickPhase);
    shoulderPitchAdjust = kneePitch;
    float anklePitchStart = 0;
    if (kickT >= BACK_PHASE + anklePitchStart)
    {
      anklePitch = interpolateSmooth(anklePitchRetracted, anklePitchAmp,
                                     kickT - BACK_PHASE - anklePitchStart,
                                     kickPhase - anklePitchStart);
    }
    // Hold...
  }

  else if (kickT < (BACK_PHASE + kickPhase + THROUGH_PHASE))
  {
    forwardDist = kickAmp;
    side = sideAmp;
    shoulderRoll = shoulderRollAmp;
    kneePitch = -kneePitchAmp;
    anklePitch = anklePitchAmp;
    shoulderPitchAdjust = kneePitch;
    // Return foot.
  }
  else if (kickT < (BACK_PHASE + kickPhase + THROUGH_PHASE + END_PHASE))
  {
    forwardDist = interpolateSmooth(
        lastKickForward, 0, kickT - BACK_PHASE - kickPhase - THROUGH_PHASE,
        END_PHASE);
    side = interpolateSmooth(lastSide, 0,
                             kickT - BACK_PHASE - kickPhase - THROUGH_PHASE,
                             END_PHASE);
    shoulderRoll = interpolateSmooth(
        lastShoulderRollAmp, 0, kickT - BACK_PHASE - kickPhase - THROUGH_PHASE,
        END_PHASE);
    kneePitch = interpolateSmooth(
        lastKneePitch, 0, kickT - BACK_PHASE - kickPhase - THROUGH_PHASE,
        END_PHASE);
    anklePitch = interpolateSmooth(
        lastAnklePitch, 0, kickT - BACK_PHASE - kickPhase - THROUGH_PHASE,
        END_PHASE);
    shoulderPitchAdjust = kneePitch;
    footh = interpolateSmooth(lastFooth, 0.5*lastFooth, kickT - BACK_PHASE - kickPhase - THROUGH_PHASE,
        END_PHASE);
    // Shift weight back to both feet.
  }

  else if (kickT < (totalPhase + SHIFT_END_PERIOD / 4))
  {
    forwardDist = 0;
    kneePitch = 0;
    anklePitch = 0;
    shoulderPitchAdjust = kneePitch;
    double endT = totalPhase - kickT;
    float t4 = kickT - totalPhase;

    if (t4 <= (SHIFT_END_PERIOD / 4) * 0.9)
    {
      footh = interpolateSmooth(0.5 * lastFooth, 0, kickT - totalPhase, (SHIFT_END_PERIOD / 4) * 0.9);
    }
    if (t4 >= (SHIFT_END_PERIOD / 4) * 0.2)
    {
      rock = interpolateSmooth(lastRock, 0, kickT - totalPhase - (SHIFT_END_PERIOD / 4) * 0.2, (SHIFT_END_PERIOD / 4) * 0.8);
    }

    // if (t4 <= (SHIFT_END_PERIOD / 4) * 0.9)
    // {
    //   rock = interpolateSmooth(lastRock, 0, kickT - totalPhase, (SHIFT_END_PERIOD / 4) * 0.9);
    // }
    // if (t4 >= (SHIFT_END_PERIOD / 4) * 0.2)
    // {
    //   footh = interpolateSmooth(0.5*lastFooth, 0, kickT - totalPhase - (SHIFT_END_PERIOD / 4) * 0.2, (SHIFT_END_PERIOD / 4) * 0.8);
    // }

    // rock = DEG2RAD(lastRock) * parabolicStep(t4, SHIFT_END_PERIOD / 4, 0);
    // footh = DEG2RAD(0.8 * lastFooth) * parabolicStep(t4, SHIFT_END_PERIOD / 4, 0);
  }
  else
  {
    kickT = 0;
    kickFinish = true;
    isKicking = false;
    rock = 0;
    footh = 0;
    shoulderPitchAdjust = 0;
    walkOption= WALK;
    walkState = NOT_WALKING;
    request->actionType = ActionCommand::Body::WALK;
    lastKickTime = 0;
  }

  // abborting or ending a kick from these values
  if (kickT < (BACK_PHASE + kickPhase + THROUGH_PHASE))
  {
    lastKickForward = forwardDist;
    lastKneePitch = kneePitch;
    lastShoulderPitchAdjust = shoulderPitchAdjust;
    lastSide = side;
    lastFooth = footh;
    lastAnklePitch = anklePitch;
    lastRock = rock;
    lastShoulderRollAmp = shoulderRollAmp;
  }
}

void WalkingEngine::avoidFeet(float &forward, float &left, float &turn)
{
	BBox leftBox;
	leftBox.a = Vector2f((theRobotModel.limbs[Limbs::footLeft] * LeftFootLT).x(), (theRobotModel.limbs[Limbs::footLeft] * LeftFootLT).y());
	leftBox.b = Vector2f((theRobotModel.limbs[Limbs::footLeft] * LeftFootRB).x(), (theRobotModel.limbs[Limbs::footLeft] * LeftFootRB).y());
	BBox rightBox;
	rightBox.a = Vector2f((theRobotModel.limbs[Limbs::footRight] * RightFootLT).x(), (theRobotModel.limbs[Limbs::footRight] * RightFootLT).y());
	rightBox.b = Vector2f((theRobotModel.limbs[Limbs::footRight] * RightFootRB).x(), (theRobotModel.limbs[Limbs::footRight] * RightFootRB).y());
	int oldForward = forward;
	int oldLeft = left;
	bool inBox = false;
	if (forward != 0 || left != 0 || turn != 0)
	{
		Point toeFall;

		if (bodyModel.isLeftPhase)
		{
			toeFall = Point(forward + toeOffset, left + footOffset);
			inBox = intersects(rightBox, toeFall);
		}
		else
		{
			toeFall = Point(forward + toeOffset, left - footOffset);
			inBox = intersects(leftBox, toeFall);
		}
		if (inBox)
		{
			forward *=0.7;
			left *= 0.7;
			if(forward == 0 && forward == 0)
			{
				forward = 3;
			}
		}
	}
}

bool WalkingEngine::intersects(BBox box, Point& p)
{
	//cout << "Foot will land at " << p.x() << " " << p.y() << " rectangle is at: "
		//	<< box.p.x() << " " << box.p.y() << " and " << box.rrp.x() << " " << box.rrp.y() << endl;
	//immediately check if the point lies within the rectangle
	//beware of coordinate system change
	if (p.x() >= box.a.x() && p.x() <= box.b.x()){
		if (p.y() <= box.a.y() && p.y() >= box.b.y()){
			return true;
		}
	}

	Point circleDist(abs(p.x() - box.a.x()), abs(p.y() - box.a.y()));
    int rectWidth = abs(box.b.x() - box.a.x());
    int rectHeight = abs(box.b.y() - box.a.y());
    if (circleDist.x() > (rectWidth/2 + toeRadius)) { return false; }
    if (circleDist.y() > (rectHeight/2 + toeRadius)) { return false; }

    if (circleDist.x() <= (rectWidth/2)) { return true; }
    if (circleDist.y() <= (rectHeight/2)) { return true; }

    double cornerDistance = (circleDist.x() - rectWidth/2)*(circleDist.x() - rectWidth/2) +
                         (circleDist.y() - rectHeight/2)*(circleDist.y() - rectHeight/2);

    return cornerDistance <= toeRadius*toeRadius;
}

//Dribble Engine

void WalkingEngine::DribbleReset()
{
   dribbleState = DribbleEND;
   exactStepsRequested = false;
}

bool WalkingEngine::DribbleHasEnded()
{
   return (dribbleState == DribbleEND);
}

void WalkingEngine::DribbleStart(ActionCommand::Body::Foot foot)
{
   dribbleState = DribbleINIT;
   this->DribbleFoot = foot;
}

void WalkingEngine::DribblePreProcess()
{
   int direction = 1;
   bool leftTurnPhase = true;
   if (DribbleFoot == ActionCommand::Body::RIGHT)
   {
      direction = -1;
      leftTurnPhase = false;
   }

   //do transition
   if (dribbleState == DribbleINIT && bodyModel.isLeftPhase == leftTurnPhase && t == 0)
   {
      dribbleState = DribbleTURN;
   }
   else if (dribbleState == DribbleTURN && bodyModel.isLeftPhase != leftTurnPhase && t == 0)
   {
      dribbleState = DribbleFORWARD;
//      TimeWhenLastDribbleEnd = theFrameInfo.time;
   }
   else if (dribbleState == DribbleFORWARD && bodyModel.isLeftPhase == leftTurnPhase && t == 0)
   {
      dribbleState = DribbleEND;
      TimeWhenLastDribbleEnd = theFrameInfo.time;
   }

   // set request
   active.left = 0;
   if (dribbleState == DribbleTURN)
   {
      active.forward = 0;
      active.turn = direction * DEG2RAD(DRIBBLE_TURN);
   }
   else if (dribbleState == DribbleFORWARD)
   {
	   active.forward = 140;
	   active.turn = 0;
   }
   else if(dribbleState == DribbleEND)
   {
	   active.forward = 70;
	   active.turn = 0;
   }
   else
   {
	   active.forward = 1; // hack so walk doesn't stand
	   active.turn = 0;
   }
   toWalkRequest();
   exactStepsRequested = true;

//   cout  << "dribbling: " << walkEngine->t << " " << dribbleState << endl;
}
void WalkingEngine::toWalkRequest()
{
	active.actionType = ActionCommand::Body::WALK;
	active.power = 0;
	active.bend = 1;
	active.speed = 1;
}

//LineUp Engine

void WalkingEngine::LineUpStart(ActionCommand::Body::Foot foot)
{
	LineUpHasStarted = true;
   this->LineUpFoot = foot;
}

void WalkingEngine::LineUpReset() {
	LineUpHasStarted = false;
   exactStepsRequested = false;
}

bool WalkingEngine::LineUpHasEnded()
{
   // Calculate required left gap (needs to be further out for kicks)
   int leftGap = LEFT_GAP_DRIBBLE;
   if (theMotionRequest.walkRequest.kickType != WalkRequest::none)
   {
      leftGap = LEFT_GAP_KICK;
   }
   int gapY = theBallModel.estimate.position.y() - leftGap;
   if (LineUpFoot == ActionCommand::Body::RIGHT)
   {
      gapY = theBallModel.estimate.position.y() + leftGap;
   }
   int gapX;
   if(theMotionRequest.walkRequest.kickType == WalkRequest::none)
   {
	   gapX = theBallModel.estimate.position.x() - FOOT_LENGTH - DRIBBLE_FORWARD_GAP - max(forwardL,forwardR)*1000;
   }
   else
   {
	   gapX = theBallModel.estimate.position.x() - FOOT_LENGTH - KICK_FORWARD_GAP - max(forwardL,forwardR)*1000;
   }

//   if (abs(gapX) < 100 && abs(gapY) < 100 && fabs(request->body.turn) < 0.2) {
//      cout << "line up hasEnded: " << gapX << " " << gapY << " " << RAD2DEG(request->body.turn) << " " << walkEngine->t << endl;
//   }

	// std::cout<<"gapx------>"<<gapX<<", gapy------>"<<gapY<<", active.turn------>"<<RAD2DEG(active.turn)<<std::endl;
   return (abs(gapX) < FORWARD_THRESHOLD && abs(gapY) < LEFT_THRESHOLD
         && fabs(active.turn) < DEG2RAD(ROTATION_THRESHOLD));   //speed is overloaded for behaviour input turn threshold
}

void WalkingEngine::LineUpPreProcess()
{
   int forward;
   if(theMotionRequest.walkRequest.kickType == WalkRequest::none)
   {
	   forward = theBallModel.estimate.position.x() - FOOT_LENGTH - DRIBBLE_FORWARD_GAP - max(forwardL,forwardR)*1000;
   }
   else
   {
	   forward = theBallModel.estimate.position.x() - FOOT_LENGTH - KICK_FORWARD_GAP - max(forwardL,forwardR)*1000;
   }
   // Calculate required left gap (needs to be further out for kicks)
   int leftGap = LEFT_GAP_DRIBBLE;
   if (theMotionRequest.walkRequest.kickType != WalkRequest::none)
   {
      leftGap = LEFT_GAP_KICK;
   }
   int left = theBallModel.estimate.position.y() - leftGap;
   if (LineUpFoot == ActionCommand::Body::RIGHT)
   {
      left = theBallModel.estimate.position.y() + leftGap;
   }
	// std::cout<<"left------>"<<left<<std::endl;
   active.forward = sign(forward) * min(MAX_FORWARD_STEP, abs(forward));
   active.left = sign(left) * min(MAX_LEFT_STEP, abs(left));

   // don't turn further than 30 degrees away from the ball heading
   float heading = atan2(theBallModel.estimate.position.y(), theBallModel.estimate.position.x());
   if (static_cast<Angle>(active.turn - heading).normalize() > DEG2RAD(30))
   {
	   std::cout<<"1----"<<std::endl;
	   active.turn = static_cast<Angle>(DEG2RAD(30) + heading).normalize();
   }
   else if (static_cast<Angle>(active.turn - heading).normalize() < -DEG2RAD(30))
   {
	   std::cout<<"2----"<<std::endl;
      active.turn = static_cast<Angle>(-DEG2RAD(30) + heading).normalize();
   }
   active.turn = sign(active.turn) * min(MAX_TURN_STEP, fabs(active.turn / 2));
   // std::cout<<"active.turn----->"<<RAD2DEG(active.turn)<<std::endl;

   toWalkRequest();
   exactStepsRequested = true;
}

void WalkingEngine::toKickRequest()
{
	soft = theMotionRequest.walkRequest.soft;
	flash = theMotionRequest.walkRequest.flash;
	active.actionType = ActionCommand::Body::KICK;
	active.forward = 0;
	active.left = 0;
	active.turn = 0;
	active.kickDirection = crop(theMotionRequest.walkRequest.target.rotation, -60_deg, 60_deg);
	active.power = crop(theMotionRequest.walkRequest.target.translation.x(), 0.f, 1.f);
	if (theMotionRequest.walkRequest.kickType == WalkRequest::left && lastKickType != WalkRequest::left)
	{
		active.foot = ActionCommand::Body::LEFT;
		lastKickType = WalkRequest::left;
	}
	else if (theMotionRequest.walkRequest.kickType == WalkRequest::right && lastKickType != WalkRequest::right)
	{
		active.foot = ActionCommand::Body::RIGHT;
		lastKickType = WalkRequest::right;
	}
	else if (theMotionRequest.walkRequest.kickType == WalkRequest::sidewardsLeft && lastKickType != WalkRequest::sidewardsLeft)
	{
		active.foot = ActionCommand::Body::LEFT;
		lastKickType = WalkRequest::sidewardsLeft;
	}
	else if (theMotionRequest.walkRequest.kickType == WalkRequest::sidewardsRight && lastKickType != WalkRequest::sidewardsRight)
	{
		active.foot = ActionCommand::Body::RIGHT;
		lastKickType = WalkRequest::sidewardsRight;
	}
}


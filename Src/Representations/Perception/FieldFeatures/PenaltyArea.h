/**
 * @file PenaltyArea.h
 * Declaration of a struct that represents a penalty Area
 * @author <a href="mailto:jesse@tzi.de">Jesse Richter-Klug</a>
 */

#pragma once

#include "FieldFeature.h"

/**
 * @struct PenaltyArea
 * it defines a the pose of the penalty area in relativ field coords to the robot
 * the penalty area pose: position => middle of the area; rotation => looking in direction of goal
 */
STREAMABLE_WITH_BASE(PenaltyArea, FieldFeature,
{
  void draw() const;
  CHECK_FIELD_FEATURE_POSE_OF("PenaltyArea");

  PenaltyArea() = default;
  PenaltyArea(const Pose2f& pose) : FieldFeature(pose) {};

  /**
  * Assignment operator for Pose2f objects
  * @param other A Pose2f object
  * @return A reference to the object after the assignment
    */
  const PenaltyArea& operator=(const Pose2f& other)
  {
    static_cast<Pose2f&>(*this) = other;
    // validity and co are not set
    return *this;
  };

  /**
  * returns 1 of the 2 global position of this feature (in case of isValid == true)
  */
  const Pose2f getGlobalFeaturePosition() const,
});

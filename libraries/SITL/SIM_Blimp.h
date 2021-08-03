/*
   This program is free software: you can redistribute it and/or modify
   it under the terms of the GNU General Public License as published by
   the Free Software Foundation, either version 3 of the License, or
   (at your option) any later version.

   This program is distributed in the hope that it will be useful,
   but WITHOUT ANY WARRANTY; without even the implied warranty of
   MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
   GNU General Public License for more details.

   You should have received a copy of the GNU General Public License
   along with this program.  If not, see <http://www.gnu.org/licenses/>.
 */
/*
  ROV/AUV/Blimp simulator class
*/

#pragma once

#include "SIM_Aircraft.h"
#include "SIM_Motor.h"
#include "SIM_Frame.h"

namespace SITL {

struct Fins
{
  float angle;
  float last_angle;
  bool dir; //0 = up, 1 = down or 0 = forward, 1 = backward
  float vel; // velocity, in m/s
  float T; //Tangential (thrust) force, in Neutons
  float N; //Normal force, in Newtons
  float Fx; //Fx,y,z = Force in bodyframe orientation at servo position, in Newtons
  float Fy;
  float Fz;
};

/*
  a blimp simulator
 */

class Blimp : public Aircraft {
public:
    Blimp(const char *frame_str);

    /* update model by one time step */
    void update(const struct sitl_input &input) override;

    /* static object creator */
    static Aircraft *create(const char *frame_str) {
        return new Blimp(frame_str);
    }

protected:
    const struct {
        float mass = 0.02;
    } model;

    void calculate_forces(const struct sitl_input &input, Vector3f &rot_accel, Vector3f &body_accel);

    Fins fin[4];
    uint8_t n_fins;
    float K_Tan = 2.0; //Tangential and normal force multipliers
    float K_Nor = 0.0;

    Vector3f moment_of_inertia;
};

}

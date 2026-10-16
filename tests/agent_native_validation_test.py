#!/usr/bin/env python3
"""Real Lua native-input rejection and continued gameplay through the agent host.

Run only against a build containing native validation: overflow/capacity cases
are designed to be rejected before allocation or Box2D mutation.
"""
import json
import math
from pathlib import Path
import sys
import tempfile
from agent_protocol_test import Agent, actor, check, properties, successful

PROBE = r'''
NativeProbe={
 ready=false,tests_passed=0,method_rejected=0,body_rejected=0,particle_rejected=0,burst_rejected=0,
 phase="initial",last_rejection="",reference_ok=false,sentinel_ok=false,color_compat_ok=false,valid_bursts=0,
 ticks=0,destroyed=false,stale_checked=false,gravity_checked=false,
 OnStart=function(self)
  self.target=Actor.Find("target")
  self.body=self.target:GetComponent("Rigidbody");self.particles=self.target:GetComponent("ParticleSystem")
  self.original_body=self.body;self.original_particles=self.particles
  self.particles:Stop();self.ready=true
 end,
 BodyState=function(self)
  local b=self.body;local p=b:GetPosition();local v=b:GetVelocity()
  local values={p.x,p.y,v.x,v.y,b:GetRotation(),b:GetAngularVelocity(),b:GetGravityScale()}
  for _,key in ipairs({"x","y","rotation","body_type","precise","gravity_scale","density","angular_friction",
      "has_collider","has_trigger","collider_type","width","height","radius","trigger_type","trigger_width",
      "trigger_height","trigger_radius","friction","bounciness","enabled"}) do values[#values+1]=b[key] end
  return values
 end,
 ParticleState=function(self)
  local values={}
  for _,key in ipairs({"x","y","frames_between_bursts","burst_quantity","duration_frames","start_scale_min","start_scale_max",
      "start_speed_min","start_speed_max","rotation_min","rotation_max","rotation_speed_min","rotation_speed_max",
      "start_color_r","start_color_g","start_color_b","start_color_a","end_color_r","end_color_g","end_color_b","end_color_a",
      "emit_angle_min","emit_angle_max","emit_radius_min","emit_radius_max","gravity_scale_x","gravity_scale_y","drag_factor",
      "angular_drag_factor","end_scale","sorting_order","enabled","image"}) do values[#values+1]=self.particles[key] end
  return values
 end,
 Reject=function(self,label,operation,snapshot,counter)
  local before=snapshot();local ok,message=pcall(operation)
  assert(not ok,label.." accepted invalid input")
  assert(type(message)=="string" and #message>0,label.." missing rejection diagnostic")
  local after=snapshot();assert(#before==#after,label.." snapshot shape changed")
  for i,value in ipairs(before) do
   assert(value==after[i] or (type(value)=="number" and value~=value and after[i]~=after[i]),label.." partially changed native/config state")
  end
  self.tests_passed=self.tests_passed+1;self[counter]=self[counter]+1;self.last_rejection=label
 end,
 Methods=function(self)
  local b=self.body;local snapshot=function() return self:BodyState() end
  local invalid={0/0,math.huge,-math.huge}
  for _,value in ipairs(invalid) do
   self:Reject("SetPosition",function() b:SetPosition(Vector2(value,0)) end,snapshot,"method_rejected")
   self:Reject("SetVelocity",function() b:SetVelocity(Vector2(0,value)) end,snapshot,"method_rejected")
   self:Reject("AddForce",function() b:AddForce(Vector2(value,0)) end,snapshot,"method_rejected")
   self:Reject("SetUpDirection",function() b:SetUpDirection(Vector2(value,1)) end,snapshot,"method_rejected")
   self:Reject("SetRightDirection",function() b:SetRightDirection(Vector2(1,value)) end,snapshot,"method_rejected")
   self:Reject("SetRotation",function() b:SetRotation(value) end,snapshot,"method_rejected")
   self:Reject("SetAngularVelocity",function() b:SetAngularVelocity(value) end,snapshot,"method_rejected")
   self:Reject("SetGravityScale",function() b:SetGravityScale(value) end,snapshot,"method_rejected")
  end
  self:Reject("SetPosition range",function() b:SetPosition(Vector2(1e9,0)) end,snapshot,"method_rejected")
  self:Reject("velocity squared norm",function() b:SetVelocity(Vector2(1e30,0)) end,snapshot,"method_rejected")
  self:Reject("force integration norm",function() b:AddForce(Vector2(1e30,0)) end,snapshot,"method_rejected")
  self:Reject("gravity derived range",function() b:SetGravityScale(1e30) end,snapshot,"method_rejected")
  self:Reject("rotation before float narrowing",function() b:SetRotation(1e300) end,snapshot,"method_rejected")
  self:Reject("angular velocity before float narrowing",function() b:SetAngularVelocity(1e300) end,snapshot,"method_rejected")
  self:Reject("gravity before float narrowing",function() b:SetGravityScale(1e300) end,snapshot,"method_rejected")
  -- Zero direction preserves the established no-op behavior.
  local angle=b:GetRotation();b:SetUpDirection(Vector2(0,0));b:SetRightDirection(Vector2(0,0))
  assert(b:GetRotation()==angle,"zero direction changed rotation")
  self.phase="methods_checked"
 end,
 BodyFields=function(self)
  local b=self.body;local snapshot=function() return self:BodyState() end
  for _,field in ipairs({"x","y","rotation","gravity_scale","density","angular_friction","width","height","radius",
      "trigger_width","trigger_height","trigger_radius","friction","bounciness"}) do
   for _,value in ipairs({0/0,math.huge,1e300,"3"}) do
    self:Reject("body field "..field,function() b[field]=value end,snapshot,"body_rejected")
   end
  end
  for _,entry in ipairs({{"x",1e9},{"y",-1e9},{"gravity_scale",1e30},{"width",0},{"height",-1},{"radius",1e7},
      {"trigger_width",0},{"trigger_height",-1},{"trigger_radius",1e7},{"density",-1},{"angular_friction",-1},
      {"friction",-1},{"bounciness",-1},{"body_type","flying"},{"collider_type","hexagon"},{"trigger_type","capsule"},
      {"precise","yes"},{"has_collider",1},{"has_trigger",0},{"enabled","true"}}) do
   local key,value=entry[1],entry[2]
   self:Reject("body domain "..key,function() b[key]=value end,snapshot,"body_rejected")
  end
  self.phase="body_fields_checked"
 end,
 ParticleFields=function(self)
  local p=self.particles;local snapshot=function() return self:ParticleState() end
  for _,field in ipairs({"frames_between_bursts","burst_quantity","duration_frames","start_color_r","start_color_g",
      "start_color_b","start_color_a","end_color_r","end_color_g","end_color_b","end_color_a","sorting_order"}) do
   for _,value in ipairs({1.5,2147483648,0/0,math.huge,"2"}) do
    self:Reject("particle integer "..field,function() p[field]=value end,snapshot,"particle_rejected")
   end
  end
  for _,field in ipairs({"frames_between_bursts","burst_quantity","duration_frames"}) do
   for _,value in ipairs({0,-1,1000001,2147483647}) do
    self:Reject("particle cadence "..field,function() p[field]=value end,snapshot,"particle_rejected")
   end
  end
  for _,field in ipairs({"x","y","start_scale_min","start_scale_max","start_speed_min","start_speed_max","rotation_min",
      "rotation_max","rotation_speed_min","rotation_speed_max","emit_angle_min","emit_angle_max","emit_radius_min",
      "emit_radius_max","gravity_scale_x","gravity_scale_y","drag_factor","angular_drag_factor"}) do
   for _,value in ipairs({0/0,math.huge,1e300,"2"}) do
    self:Reject("particle float "..field,function() p[field]=value end,snapshot,"particle_rejected")
   end
  end
  for _,value in ipairs({math.huge,1e300,"none"}) do
   self:Reject("end_scale invalid",function() p.end_scale=value end,snapshot,"particle_rejected")
  end
  p.end_scale=0/0;self.sentinel_ok=p.end_scale~=p.end_scale
  assert(self.sentinel_ok,"documented unset end_scale sentinel was rejected")
  -- Endpoints may be edited in either order. Cross-field rejection belongs to
  -- consumption, so restore the valid range inside this callback before the
  -- engine's next automatic ParticleSystem.OnUpdate.
  p.start_scale_min=2
  assert(p.start_scale_min==2,"single valid endpoint was rejected prematurely")
  self:Reject("Burst inverted scale",function() p:Burst() end,snapshot,"burst_rejected")
  p.start_scale_min=1
  p.duration_frames=1000000;p.frames_between_bursts=1;p.burst_quantity=2
  self:Reject("Burst reserve overflow bound",function() p:Burst() end,snapshot,"burst_rejected")
  p.duration_frames=3;p.frames_between_bursts=10;p.burst_quantity=1
  -- Preserve legacy color behavior: start channels clamp on emission, while
  -- out-of-byte end channels mean interpolation is disabled. The native suite
  -- inspects emitted values; this host test verifies those values remain usable.
  p.start_color_r=-1;p.start_color_a=256;p.end_color_b=-2;p.end_color_a=256
  self.color_compat_ok=p.start_color_r==-1 and p.start_color_a==256 and p.end_color_b==-2 and p.end_color_a==256
  assert(self.color_compat_ok,"legacy color configuration was changed")
  p:Burst();self.valid_bursts=self.valid_bursts+1
  self.phase="particles_checked"
 end,
 ValidUpdates=function(self)
  local b=self.body;local p=self.particles
  b:SetPosition(Vector2(3,4));assert(b:GetPosition().x==3 and b:GetPosition().y==4,"valid teleport did not apply")
  b:SetRotation(90);assert(math.abs(b:GetRotation()-90)<0.001,"valid rotation did not apply")
  b:SetUpDirection(Vector2(0,-1));b:SetRightDirection(Vector2(1,0))
  b:SetAngularVelocity(15);b:SetGravityScale(0);b:SetVelocity(Vector2(0.5,0.25));b:AddForce(Vector2(0.1,0))
  -- Raw fields remain author configuration; these safe updates need not rebuild
  -- an already-live Box2D fixture immediately.
  b.width=2;b.height=2;b.friction=0.6;b.density=2
  assert(b.width==2 and b.height==2 and math.abs(b.friction-0.6)<0.00001 and b.density==2,"valid raw config update lost")
  p.duration_frames=4;p.frames_between_bursts=2;p.burst_quantity=2
  p.start_color_r=20;p.end_color_g=100;p.end_scale=0.5;p.drag_factor=0.9
  p:Burst();p:Stop();self.valid_bursts=self.valid_bursts+1
  assert(p.duration_frames==4 and p.burst_quantity==2 and p.end_scale==0.5,"valid particle update lost")
  self.phase="valid_updates"
 end,
 OnUpdate=function(self)
  self.ticks=self.ticks+1
  if Input.GetKeyDown("m") then self:Methods() end
  if Input.GetKeyDown("f") then self:BodyFields() end
  if Input.GetKeyDown("p") then self:ParticleFields() end
  if Input.GetKeyDown("v") then self:ValidUpdates() end
  if Input.GetKeyDown("g") then
   self:Reject("huge finite gravity",function() self.body:SetGravityScale(1e30) end,function() return self:BodyState() end,"method_rejected")
   self.gravity_checked=true
  end
  if Input.GetKeyDown("e") then self.body.gravity_scale=0/0 end
  if Input.GetKeyDown("x") then
   self.destroy_x=self.body:GetPosition().x;self.destroy_y=self.body:GetPosition().y
   Actor.Destroy(self.target);self.destroyed=true;self.phase="destroyed"
  end
  if Input.GetKeyDown("z") then
   assert(self.destroyed and not self.target:IsValid(),"target generation unexpectedly remains live")
   -- Preserve established no-op lifecycle/liveness behavior. An explicit
   -- expired-object error is also safe, but nothing may recreate a native body.
   pcall(function() self.body:OnStart();self.body:SetPosition(Vector2(50,0));self.body:SetVelocity(Vector2(1,0)) end)
   pcall(function() self.particles:OnStart();self.particles:Play();self.particles:OnUpdate();self.particles:Burst() end)
   assert(Physics.Raycast(Vector2(48,0),Vector2(1,0),4)==nil,"stale body resurrected at its newly requested position")
   assert(Physics.Raycast(Vector2(self.destroy_x-2,self.destroy_y),Vector2(1,0),4)==nil,"destroyed body left a native fixture")
   self.stale_checked=true;self.phase="stale_checked"
  end
  self.reference_ok=self.body==self.original_body and self.particles==self.original_particles
  if not self.destroyed then
   assert(self.target:IsValid() and self.body.actor:IsValid() and self.particles.actor:IsValid(),"validation invalidated native ownership")
  end
 end}
'''


def fixture(project):
    for directory in ["component_types", "scenes"]:
        (project / directory).mkdir(parents=True)
    (project / "game.config").write_text(json.dumps({"initial_scene": "native"}))
    (project / "component_types/NativeProbe.lua").write_text(PROBE)
    (project / "scenes/native.scene").write_text(json.dumps({"actors": [
        {"uid": 1, "name": "guardian", "components": {"probe": {"type": "NativeProbe"}}},
        {"uid": 2, "name": "target", "components": {
            "body": {"type": "Rigidbody", "gravity_scale": 0, "has_trigger": False, "collider_type": "circle", "radius": 0.25},
            "particles": {"type": "ParticleSystem", "duration_frames": 3, "frames_between_bursts": 10, "burst_quantity": 1,
                          "start_speed_min": 0, "start_speed_max": 0, "emit_radius_min": 0, "emit_radius_max": 0},
            "transform": {"type": "Transform"}}}]}))


def finite(value):
    if isinstance(value, float):
        check(math.isfinite(value), "non-finite JSON observation escaped native validation")
    elif isinstance(value, dict):
        for child in value.values():
            finite(child)
    elif isinstance(value, list):
        for child in value:
            finite(child)


def stats(snapshot):
    return properties(snapshot, "guardian", "NativeProbe")


def step(agent, frames=1, keys=()):
    observed = successful(agent, {"cmd": "step", "frames": frames, "keys": list(keys)})
    finite(observed)
    return observed


def run(runtime, project):
    agent = Agent(str(runtime), str(project))
    try:
        initial = successful(agent, {"cmd": "observe"});finite(initial)
        first = step(agent)
        check(stats(first)["ready"] and stats(first)["reference_ok"], "real native references did not initialize")
        methods = step(agent, keys=["m"])
        check(stats(methods)["phase"] == "methods_checked" and stats(methods)["method_rejected"] >= 31,
              "method rejection matrix did not execute")
        check(abs(actor(methods, "target")["position"]["x"]) < 0.00001 and
              abs(actor(methods, "target")["position"]["y"]) < 0.00001,
              "rejected force/velocity affected the next real physics tick")
        fields = step(agent, keys=["f"])
        check(stats(fields)["phase"] == "body_fields_checked" and stats(fields)["body_rejected"] >= 76,
              "raw Rigidbody rejection matrix did not execute")
        particles = step(agent, keys=["p"])
        check(stats(particles)["phase"] == "particles_checked" and stats(particles)["particle_rejected"] >= 147
              and stats(particles)["burst_rejected"] == 2 and stats(particles)["sentinel_ok"] and stats(particles)["color_compat_ok"],
              "particle field/configuration matrix did not execute")
        check(properties(particles, "target", "ParticleSystem")["end_scale"] is None,
              "unset end_scale sentinel should remain the documented null observation")
        valid = step(agent, keys=["v"])
        check(stats(valid)["valid_bursts"] == 2 and stats(valid)["reference_ok"], "valid native operations failed after rejections")
        before_x = actor(valid, "target")["position"]["x"]
        moved = step(agent, 3)
        check(actor(moved, "target")["position"]["x"] > before_x,
              "retained live body did not move after valid velocity/force")
        huge = step(agent, keys=["g"])
        check(stats(huge)["gravity_checked"] and properties(huge, "target", "Rigidbody")["gravity_scale"] == 0,
              "finite huge gravity mutated configuration")
        stable = step(agent, 3)
        check(stats(stable)["reference_ok"], "world did not remain usable for three post-gravity ticks")
        # One intentionally unprotected bad setter tests the actual error path:
        # it may abort this callback, but must not corrupt native state or kill the worker.
        error = agent.request({"cmd": "step", "keys": ["e"]})
        check(not error["ok"] and not error.get("requires_reset", False), "ordinary native argument error killed the worker")
        observed = successful(agent, {"cmd": "observe"});finite(observed)
        check(properties(observed, "target", "Rigidbody")["gravity_scale"] == 0,
              "uncaught setter error left a non-finite native value")
        continued = step(agent, 3)
        check(continued["frame"] == error["frame"] + 3, "later valid steps were poisoned by an old argument error")
        destroyed = step(agent, keys=["x"])
        check(not any(item["name"] == "target" for item in destroyed["actors"]), "target actor was not removed")
        stale = step(agent, keys=["z"])
        check(stats(stale)["stale_checked"] and stats(stale)["reference_ok"], "stale-reference liveness checks did not execute")
        step(agent, 3)
        reset = successful(agent, {"cmd": "reset"})
        check(reset == initial, "fresh worker reset did not restore valid initial native configuration")
        step(agent)
        successful(agent, {"cmd": "quit"})
    finally:
        agent.close()


if __name__ == "__main__":
    check(len(sys.argv) == 2, "usage: agent_native_validation_test.py RUNTIME")
    with tempfile.TemporaryDirectory(prefix="pocket-native-validation-") as directory:
        project = Path(directory) / "project";fixture(project)
        run(Path(sys.argv[1]).resolve(), project)
    print("agent_native_validation_test: rejected inputs preserve state; valid physics/particles, finite continuation and stale lifecycle passed")

#include "adaptive_flow_controller.hpp"
#include <cassert>
#include <chrono>
#include <cmath>
#include <iostream>
#include <string>
using namespace std::chrono_literals;
AdaptiveFlowObservation sample() {
 AdaptiveFlowObservation s{}; s.elapsed=100ms; s.frameBudgetMs=50; s.totalLsfgMs=8; s.flowMs=5;
 s.generationCount=3; s.sourceFps=30; s.outputFps=90; s.outputTargetFps=120;
 s.outputCadenceValid=true; s.outputTargeted=true; s.outputTargetSatisfied=false; s.outputDeficit=true;
 s.valid=true; return s;
}
int main(int argc,char**argv){
 assert(argc==2); const std::string name=argv[1]; AdaptiveFlowController c(AdaptiveFlowPreset::Quality); auto s=sample();
 if(name=="unprofitable_target") { for(int i=0;i<3000;i++)c.observe(s); assert(std::abs(c.currentScale()-1.f)<.0001f); }
 else if(name=="wsi_contribution") {s.flowMs=.05;s.totalLsfgMs=.1;s.wsiPresentationPressure=true; int changes=0;for(int i=0;i<100;i++){c.observe(s);changes+=c.telemetry().changed;}assert(changes==0);}
 else if(name=="known_tiny_flow") {s.generatedCadenceSample=true;s.flowMs=.05;s.totalLsfgMs=.1;int changes=0;for(int i=0;i<100;i++){c.observe(s);changes+=c.telemetry().changed;}assert(changes==0);}
 else if(name=="unknown_output_recovery") {assert(c.seedCurrentScale(.8f));s.outputCadenceValid=false;s.outputDeficit=false;s.outputTargetSatisfied=true;for(int i=0;i<100;i++)c.observe(s);assert(c.currentScale()==.8f);}
 else if(name=="history_provenance") {assert(c.seedCurrentScale(.8f));s.generatedWorkSample=false;s.outputTargeted=false;s.outputDeficit=false;s.globalPressureValid=true;s.globalGpuUsagePercent=60;for(int i=0;i<100;i++)c.observe(s);assert(c.currentScale()==.8f);}
 else if(name=="rejected_probe_hold") {int changes=0;for(int i=0;i<3000;i++){c.observe(s);changes+=c.telemetry().changed;}assert(changes==2);s.sourceFps=36;s.totalLsfgMs=5;s.flowMs=3;for(int i=0;i<100;i++){c.observe(s);changes+=c.telemetry().changed;}assert(changes>2);}
 else if(name=="rejected_recovery_hold") {assert(c.seedCurrentScale(.8f));s.outputTargetSatisfied=true;s.outputDeficit=false;s.adaptiveFramegenMode=true;s.scheduledGenerationDensity=3;int changes=0;for(int i=0;i<3000;i++){s.sourceFps=c.currentScale()>.8f?25:30;s.scheduledGenerationDensity=c.currentScale()>.8f?4:3;c.observe(s);changes+=c.telemetry().changed;}assert(changes==2);}
 else if(name=="cadence_without_gpu") {s.fixedMultiplierMode=true;s.generatedCadenceSample=true;s.generatedWorkSample=false;s.totalLsfgMs=0;s.flowMs=0;s.outputTargeted=false;s.outputDeficit=false;int changes=0;for(int i=0;i<3000;i++){c.observe(s);changes+=c.telemetry().changed;}assert(changes==2);assert(c.currentScale()==1.f);}
 else if(name=="measured_benefit") {int changes=0;for(int i=0;i<100;i++){s.sourceFps=c.currentScale()<1.f?34:30;s.outputFps=c.currentScale()<1.f?110:90;s.flowMs=c.currentScale()<1.f?4:5;c.observe(s);changes+=c.telemetry().changed;}assert(c.currentScale()<1.f);assert(changes>0);}
 else return 2;
 std::cout<<name<<" passed\n";
}

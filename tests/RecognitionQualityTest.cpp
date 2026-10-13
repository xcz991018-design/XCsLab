#include "RecognitionQuality.hpp"
#include <iostream>

PointerDetectionResult fixture(int count, bool curvedTail=false) {
    PointerDetectionResult result;
    result.valid=true; result.headEnd='A'; result.tipLikelihoodA=12; result.tipLikelihoodB=2;
    auto& region=result.directionA.candidateRegion;
    region.commonStartRadius=20; region.commonEndRadius=20+2*(count-1);
    result.directionA.reach=region.commonEndRadius;
    result.directionA.medianWidth=8;
    for (int i=0;i<count;++i) {
        RadialSection s; s.valid=true; s.radius=20+2*i; s.width=8; s.leftT=-4; s.rightT=4;
        result.sections.push_back(s);
        EdgePoint p; p.radius=s.radius; p.hasRawSupport=true;
        region.leftEdge.points.push_back(p); region.rightEdge.points.push_back(p);
        if (curvedTail) {
            s.leftT=-4-0.002*i*i; s.rightT=4+0.002*i*i; s.width=s.rightT-s.leftT;
            result.tailProfile.sections.push_back(s);
        }
    }
    result.tailDetected=curvedTail; result.tailProfile.reach=40; result.tailProfile.medianWidth=12;
    return result;
}
int main() {
    auto straight=fixture(30,true);
    auto good=evaluateRecognitionQuality(straight,{});
    if (good["status"]!="trusted" || !good["tail"].toObject()["available"].toBool()) return 1;
    auto weak=fixture(3);
    if (evaluateRecognitionQuality(weak,{})["status"]!="unassessable") return 1;
    straight.tipLikelihoodB=11;
    if (evaluateRecognitionQuality(straight,{})["status"]!="review") return 1;
    straight.tipLikelihoodB=2;
    for (int i=8;i<18;++i) straight.directionA.candidateRegion.rightEdge.points[i].hasRawSupport=false;
    auto gap=evaluateRecognitionQuality(straight,{});
    if (!gap["body"].toObject()["gap_warning"].toBool() || gap["status"]!="review") return 1;
    const auto config=resolvedQualityConfig({{"body_weights",QJsonArray{0,0,0,0,0}}, {"coverage_range",QJsonArray{1,0}}});
    if (config["body_weights"]!=defaultQualityConfig()["body_weights"]) return 1;
    std::cout << "Straight body, curved tail, insufficient evidence, ambiguous direction and gaps passed\n";
    return 0;
}

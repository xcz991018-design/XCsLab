#pragma once
#include "PointerDetector.hpp"
#include <QJsonObject>
#include <QJsonArray>
#include <algorithm>
#include <cmath>

// Unicode escapes keep wide literals independent of the MSVC source code page.
// Advisory quality scores only. Never used by candidate selection or success.
inline QJsonObject defaultQualityConfig() {
    return {{"version", "quality-v1"}, {"body_weights", QJsonArray{35,20,15,20,10}},
        {"tail_weights", QJsonArray{40,20,15,15,10}}, {"direction_weights", QJsonArray{70,30}},
        {"support_range", QJsonArray{0.40,0.85}}, {"coverage_range", QJsonArray{0.40,0.85}},
        {"continuous_range", QJsonArray{0.15,0.60}}, {"relative_margin_range", QJsonArray{0.10,0.50}},
        {"absolute_margin_range", QJsonArray{1.0,4.0}}, {"min_supported_sections", 5},
        {"min_adjacent_pairs", 3}, {"max_gap_ratio", 0.20}, {"trusted_score", 80},
        {"fit_residual_width_range", QJsonArray{0.05,0.30}},
        {"width_step_range", QJsonArray{0.05,0.30}}, {"curve_change_range", QJsonArray{0.05,0.40}}};
}
inline QJsonObject resolvedQualityConfig(const QJsonObject& overrides) {
    auto config = defaultQualityConfig();
    for (auto it = overrides.begin(); it != overrides.end(); ++it) {
        if (!config.contains(it.key()) || it.key() == "version") continue;
        if (config[it.key()].isArray()) {
            const auto a = it.value().toArray();
            const auto expected = config[it.key()].toArray();
            bool valid = a.size() == expected.size();
            double sum = 0;
            for (const auto& v : a) { valid &= v.isDouble() && std::isfinite(v.toDouble()) && v.toDouble() >= 0; sum += v.toDouble(); }
            if (it.key().endsWith("range")) valid &= a.size() == 2 && a[1].toDouble() > a[0].toDouble();
            else valid &= sum > 0;
            if (valid) config[it.key()] = a;
        } else if (it.value().isDouble() && std::isfinite(it.value().toDouble()) && it.value().toDouble() > 0) {
            const double v = it.value().toDouble();
            if (it.key() == "max_gap_ratio" ? v <= 1 : it.key() == "trusted_score" ? v <= 100 : v <= 10000) config[it.key()] = v;
        }
    }
    return config;
}
inline double qualityRamp(double x, const QJsonArray& range, bool reverse = false) {
    double q = std::clamp((x-range[0].toDouble())/(range[1].toDouble()-range[0].toDouble()), 0.0, 1.0);
    return 100.0 * (reverse ? 1-q : q);
}
inline QJsonObject evaluateRecognitionQuality(const PointerDetectionResult& result, const QJsonObject& overrides) {
    const auto cfg = resolvedQualityConfig(overrides);
    QJsonArray reasons;
    auto reason = [&](const QString& code, const QString& text) { reasons.append(QJsonObject{{"code",code},{"message",text}}); };
    auto evaluate = [&](const std::vector<RadialSection>& sections, const PointerCandidateRegion* region, bool tail) {
        QJsonObject metrics, scores;
        std::vector<const RadialSection*> valid;
        for (const auto& s : sections) if (s.valid) valid.push_back(&s);
        const double step = 2.0; // Current GaugeConfig body and tail sampling.
        if (valid.size() < 2) return QJsonObject{{"available",false},{"metrics",metrics},{"scores",scores}};
        double start = valid.front()->radius, end = valid.back()->radius;
        if (region) { start = std::max(start, region->commonStartRadius); end = std::min(end, region->commonEndRadius); }
        const int expected = std::max(1, int(std::round((end-start)/step))+1);
        int supported = 0, leftCount = 0, rightCount = 0, longest = 0, run = 0, gap = 0, maxGap = 0, covered = 0, pairs = 0, curvePairs = 0;
        double widthChange = 0, curveChange = 0, sumWidth = 0;
        const RadialSection* previous = nullptr;
        double lastSlopeL = 0, lastSlopeR = 0;
        bool hasSlope = false;
        auto supportedAt = [&](const EdgeTrack& track, double r) {
            for (const auto& p : track.points) if (std::abs(p.radius-r) < step*0.25 && p.hasRawSupport) return true;
            return false;
        };
        for (int i=0; i<expected; ++i) {
            const double r = start+i*step;
            const RadialSection* current = nullptr;
            for (const auto* s : valid) if (std::abs(s->radius-r) < step*0.25) { current=s; break; }
            const bool left = current && (!region || supportedAt(region->leftEdge,r));
            const bool right = current && (!region || supportedAt(region->rightEdge,r));
            leftCount += left; rightCount += right;
            if (left && right) ++supported;
            if (!current || !left || !right) { ++gap; maxGap=std::max(maxGap,gap); run=0; previous=nullptr; hasSlope=false; continue; }
            ++covered; gap=0; longest=std::max(longest,++run); sumWidth+=current->width;
            // Arc caps may close rapidly; keep them in coverage statistics,
            // but exclude the last 15% from tail width and curvature scoring.
            if (previous && (!tail || r <= start+0.85*(end-start))) {
                const double dr=current->radius-previous->radius;
                const double w=std::max(1.0,(current->width+previous->width)*0.5);
                widthChange+=std::abs(current->width-previous->width)/w*step/dr;
                const double sl=(current->leftT-previous->leftT)/dr, sr=(current->rightT-previous->rightT)/dr;
                if (hasSlope) { curveChange+=std::max(std::abs(sl-lastSlopeL),std::abs(sr-lastSlopeR)); ++curvePairs; }
                lastSlopeL=sl; lastSlopeR=sr; hasSlope=true; ++pairs;
            }
            previous=current;
        }
        const double support=double(supported)/expected, coverage=double(covered)/expected;
        const double continuous=double(longest)/expected, gapRatio=double(maxGap)/expected;
        const double meanWidth=covered ? sumWidth/covered : 0;
        metrics={{"evaluated_span_px",std::max(0.0,end-start)}, {"expected_sections",expected},
            {"supported_sections",supported},{"adjacent_pairs",pairs},{"left_support",double(leftCount)/expected},
            {"right_support",double(rightCount)/expected},{"pair_support",support},{"coverage",coverage},
            {"continuous_ratio",continuous},{"missing_ratio",1-coverage},{"max_gap_ratio",gapRatio},
            {"mean_width_px",meanWidth},{"width_step_relative",pairs ? widthChange/pairs : 0},
            {"curve_change",curvePairs ? curveChange/curvePairs : 0}};
        scores["support"]=qualityRamp(support,cfg["support_range"].toArray());
        scores["coverage"]=qualityRamp(coverage,cfg["coverage_range"].toArray());
        scores["continuous"]=qualityRamp(continuous,cfg["continuous_range"].toArray());
        scores["width"]=qualityRamp(pairs ? widthChange/pairs : 1,cfg["width_step_range"].toArray(),true);
        double shape=0;
        if (tail) shape=qualityRamp(curvePairs ? curveChange/curvePairs : 1,cfg["curve_change_range"].toArray(),true);
        else if (region) {
            const double residual=std::max(region->leftEdge.lineFitResidual,region->rightEdge.lineFitResidual);
            metrics["fit_residual_px"]=residual;
            metrics["fit_residual_width_ratio"]=residual/std::max(1.0,meanWidth);
            shape=qualityRamp(residual/std::max(1.0,meanWidth),cfg["fit_residual_width_range"].toArray(),true);
        }
        scores["shape"]=shape;
        const auto weights=cfg[tail ? "tail_weights" : "body_weights"].toArray();
        const double values[]={scores["support"].toDouble(),scores["coverage"].toDouble(),scores["continuous"].toDouble(),shape,scores["width"].toDouble()};
        double total=0, weightSum=0;
        for (int i=0;i<5;++i) { total+=values[i]*weights[i].toDouble(); weightSum+=weights[i].toDouble(); }
        const bool available=supported>=cfg["min_supported_sections"].toInt() && pairs>=cfg["min_adjacent_pairs"].toInt() && meanWidth>=1.5;
        return QJsonObject{{"available",available},{"score",available ? QJsonValue(total/weightSum) : QJsonValue(QJsonValue::Null)},
            {"metrics",metrics},{"scores",scores},{"gap_warning",gapRatio>cfg["max_gap_ratio"].toDouble()}};
    };
    const auto& body=result.headEnd=='A' ? result.directionA : result.directionB;
    const auto bodyQuality=evaluate(result.sections,&body.candidateRegion,false);
    const auto tailQuality=result.tailDetected ? evaluate(result.tailProfile.sections,nullptr,true) : QJsonObject{{"available",false}};
    const double a=result.tipLikelihoodA, b=result.tipLikelihoodB;
    const double absolute=std::abs(a-b), relative=absolute/std::max(1e-9,a+b);
    const double relScore=qualityRamp(relative,cfg["relative_margin_range"].toArray());
    const double absScore=qualityRamp(absolute,cfg["absolute_margin_range"].toArray());
    const auto dw=cfg["direction_weights"].toArray();
    const double direction=(relScore*dw[0].toDouble()+absScore*dw[1].toDouble())/(dw[0].toDouble()+dw[1].toDouble());
    const bool available=bodyQuality["available"].toBool();
    if (!result.valid) reason("NO_VALID_CANDIDATE",QStringLiteral("\u539f\u68c0\u6d4b\u7b97\u6cd5\u672a\u627e\u5230\u6709\u6548\u6307\u9488\u5019\u9009"));
    if (!available) reason("INSUFFICIENT_BODY_EVIDENCE",QStringLiteral("\u9488\u8eab\u771f\u5b9e\u53cc\u4fa7\u8bc1\u636e\u6216\u8fde\u7eed\u622a\u9762\u4e0d\u8db3"));
    if (bodyQuality["gap_warning"].toBool()) reason("EXCESSIVE_EDGE_GAP",QStringLiteral("\u9488\u8eab\u6700\u957f\u8fde\u7eed\u8fb9\u7f18\u7f3a\u5931\u8d85\u8fc7\u8b66\u544a\u9608\u503c"));
    if (available && bodyQuality["score"].toDouble()<cfg["trusted_score"].toDouble()) reason("LOW_BODY_QUALITY",QStringLiteral("\u9488\u8eab\u8d28\u91cf\u5206\u4f4e\u4e8e\u53ef\u4fe1\u95e8\u69db"));
    const auto bodyScores=bodyQuality["scores"].toObject();
    for (const auto& key : {QString("support"),QString("coverage"),QString("continuous"),QString("shape"),QString("width")}) {
        if (available && bodyScores[key].toDouble()<cfg["trusted_score"].toDouble())
            reason("BODY_"+key.toUpper()+"_LOW",QStringLiteral("\u9488\u8eab\u5206\u9879 %1 \u504f\u4f4e").arg(key));
    }
    if (direction<cfg["trusted_score"].toDouble()) reason("DIRECTION_AMBIGUOUS",QStringLiteral("\u6b63\u53cd\u65b9\u5411\u9886\u5148\u5e45\u5ea6\u4e0d\u8db3"));
    if (result.tailDetected && !tailQuality["available"].toBool()) reason("TAIL_EVIDENCE_INSUFFICIENT",QStringLiteral("\u9488\u5c3e\u8bc1\u636e\u4e0d\u8db3\uff0c\u5df2\u6709\u65b9\u5411\u52a0\u5206\u9700\u590d\u6838"));
    if (result.tailDetected && tailQuality["available"].toBool() &&
        (tailQuality["score"].toDouble()<cfg["trusted_score"].toDouble() || tailQuality["gap_warning"].toBool()))
        reason("TAIL_QUALITY_LOW",QStringLiteral("\u9488\u5c3e\u8d28\u91cf\u4e0d\u8db3\uff0c\u5df2\u6709\u65b9\u5411\u52a0\u5206\u9700\u590d\u6838"));
    const double overall=available ? std::min(bodyQuality["score"].toDouble(),direction) : 0;
    const bool reviewRequired=overall<cfg["trusted_score"].toDouble() || bodyQuality["gap_warning"].toBool() ||
        (result.tailDetected && (!tailQuality["available"].toBool() || tailQuality["score"].toDouble()<cfg["trusted_score"].toDouble() || tailQuality["gap_warning"].toBool()));
    const QString status=!result.valid || !available ? "unassessable" : reviewRequired ? "review" : "trusted";
    return {{"version","quality-v1"},{"advisory_only",true},{"status",status},{"overall",available ? QJsonValue(overall) : QJsonValue(QJsonValue::Null)},
        {"body",bodyQuality},{"tail",tailQuality},{"direction",QJsonObject{{"score",direction},{"relative_margin",relative},{"absolute_margin",absolute},
            {"relative_score",relScore},{"absolute_score",absScore},{"body_reach_px",body.reach},{"tail_reach_px",result.tailProfile.reach},
            {"body_tail_reach_ratio",result.tailDetected && result.tailProfile.reach>0 ? QJsonValue(body.reach/result.tailProfile.reach) : QJsonValue(QJsonValue::Null)},
            {"tail_body_width_ratio",result.tailDetected ? QJsonValue(result.tailProfile.medianWidth/std::max(1e-9,body.medianWidth)) : QJsonValue(QJsonValue::Null)}}},
        {"reasons",reasons},{"config",cfg}};
}
inline QString qualitySummary(const QJsonObject& quality) {
    if (quality.isEmpty()) return QStringLiteral("\u8d28\u91cf\uff1a\u672a\u8bc4\u4f30");
    auto score=[](const QJsonObject& item) { return item["score"].isDouble() ? QString::number(item["score"].toDouble(),'f',1) : QStringLiteral("\u4e0d\u53ef\u8bc4\u4f30"); };
    const auto status=quality["status"].toString();
    QString text=QStringLiteral("\u65c1\u8def\u8d28\u91cf\uff1a%1 | \u9488\u8eab %2 | \u9488\u5c3e %3 | \u671d\u5411 %4")
        .arg(status=="trusted" ? QStringLiteral("\u53ef\u4fe1") : status=="review" ? QStringLiteral("\u9700\u590d\u6838") : QStringLiteral("\u4e0d\u53ef\u8bc4\u4f30"))
        .arg(score(quality["body"].toObject()),score(quality["tail"].toObject()),score(quality["direction"].toObject()));
    for (const auto& r : quality["reasons"].toArray()) text+="\n"+r.toObject()["message"].toString();
    return text;
}
inline QString qualityDetails(const QJsonObject& quality) {
    QString text=qualitySummary(quality);
    for (const auto& name : {QString("body"),QString("tail")}) {
        const auto part=quality[name].toObject();
        const auto m=part["metrics"].toObject(), s=part["scores"].toObject();
        if (m.isEmpty()) continue;
        text+=QStringLiteral("\n%1\u5206\u9879\uff1a\u652f\u6301 %2 / \u8986\u76d6 %3 / \u8fde\u7eed %4 / \u5f62\u72b6 %5 / \u5bbd\u5ea6 %6")
            .arg(name=="body" ? QStringLiteral("\u9488\u8eab") : QStringLiteral("\u9488\u5c3e"))
            .arg(s["support"].toDouble(),0,'f',1).arg(s["coverage"].toDouble(),0,'f',1)
            .arg(s["continuous"].toDouble(),0,'f',1).arg(s["shape"].toDouble(),0,'f',1).arg(s["width"].toDouble(),0,'f',1);
        text+=QStringLiteral("\n\u53cc\u4fa7\u771f\u5b9e\u652f\u6301 %1%\uff0c\u6709\u6548\u8986\u76d6 %2%\uff0c\u6700\u957f\u7f3a\u5931 %3%")
            .arg(100*m["pair_support"].toDouble(),0,'f',1).arg(100*m["coverage"].toDouble(),0,'f',1)
            .arg(100*m["max_gap_ratio"].toDouble(),0,'f',1);
    }
    return text;
}

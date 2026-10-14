#include "GaugeTemplateStore.hpp"
#include "GaugeProfile.hpp"
#include "MySqlRepository.hpp"
#include <QJsonArray>
#include <QJsonDocument>
#include <cmath>

GaugeTemplateStore::GaugeTemplateStore(GaugeTemplateBackend backend) : m_backend(std::move(backend)) {
    if (!m_backend.load) m_backend.load = []() { return MySqlRepository().loadTemplates(); };
    if (!m_backend.save) m_backend.save = [](const QJsonObject& definition, quint64 revision) { return MySqlRepository().saveTemplate(definition, revision); };
    if (!m_backend.remove) m_backend.remove = [](const QString& id, quint64 revision) { MySqlRepository().deleteTemplate(id, revision); };
}

QString GaugeTemplateStore::idOf(const QJsonObject& profile) {
    return profile.contains("gauge_id") ? profile["gauge_id"].toString() : profile["profile_id"].toString();
}

bool GaugeTemplateStore::load(QString& error) {
    m_loaded = false;
    m_records.clear();
    error.clear();
    try {
        for (const auto& record : m_backend.load()) {
            if (record.id.isEmpty() || record.id != idOf(record.definition) || !record.revision || m_records.contains(record.id))
                throw std::runtime_error("DATABASE_TEMPLATE_INVALID");
            m_records[record.id] = record;
            m_records[record.id].definition.remove("min_angle_deg");
            m_records[record.id].definition.remove("max_angle_deg");
        }
        m_loaded = true;
        return true;
    } catch (const std::exception& e) {
        m_records.clear();
        error = QString::fromUtf8(e.what());
        return false;
    }
}

QStringList GaugeTemplateStore::ids() const { return m_loaded ? m_records.keys() : QStringList(); }
QJsonObject GaugeTemplateStore::profile(const QString& id) const { return m_loaded ? m_records.value(id).definition : QJsonObject(); }
QStringList GaugeTemplateStore::usedBy(const QString& id) const { return m_loaded ? m_records.value(id).usedBy : QStringList(); }
QVector<GaugeTemplateField> GaugeTemplateStore::fields() {
    return fields(GaugeProfile());
}

QJsonObject GaugeTemplateStore::effectiveProfile(const QString& id) const {
    GaugeProfile g;
    try {
        if (!profile(id).isEmpty()) g.loadFromJson(profile(id));
    } catch (const std::exception&) { return defaults(); }
    QJsonObject result{{"unit", QString::fromStdString(g.unit)}};
    for (const auto& field : fields(g)) result[field.key] = field.defaultValue;
    return result;
}

QVector<GaugeTemplateField> GaugeTemplateStore::fields(const GaugeProfile& g) {
    QVector<GaugeTemplateField> result;
    auto number = [&](const char* key, const char* label, int page, double value, double low, double high,
                      int decimals = 3, QStringList aliases = {}) {
        result.append({QString::fromLatin1(key), QString::fromUtf8(label), page, value, low, high, decimals, aliases});
    };
    auto flag = [&](const char* key, const char* label, int page, bool value) {
        result.append({QString::fromLatin1(key), QString::fromUtf8(label), page, value, 0, 1, 0, {}});
    };
    number("min_val", "量程下限", 0, g.min_val, -1e9, 1e9, 6);
    number("max_val", "量程上限", 0, g.max_val, -1e9, 1e9, 6);
    flag("clockwise", "数值沿顺时针方向增大", 0, g.clockwise);
    flag("pointer_invert_direction", "反转指针方向", 0, g.pointer_invert_direction);
    number("pointerBaseRadius", "中心死区半径 (像素)", 1, g.pointerBaseRadius, 0, 100000, 3, {"pointer_base_radius"});
    number("pointerMinWidth", "指针最小宽度 (像素)", 1, g.pointerMinWidth, 0.001, 100000, 3, {"pointer_min_width", "body_min_width"});
    number("pointerMaxWidth", "指针最大宽度 (像素)", 1, g.pointerMaxWidth, 0.001, 100000, 3, {"pointer_max_width", "body_max_width"});
    number("pointerMinLength", "指针最小长度 (像素)", 1, g.pointerMinLength, 0.001, 100000, 3, {"pointer_min_length"});
    number("coarseAngleStep", "粗搜索角度步长 (°)", 1, g.coarseAngleStep, 0.001, 360, 3, {"coarse_angle_step"});
    number("fineAngleStep", "精搜索角度步长 (°)", 1, g.fineAngleStep, 0.001, 360, 3, {"fine_angle_step"});
    number("fineAngleRange", "精搜索角度范围 (±°)", 1, g.fineAngleRange, 0.001, 180, 3, {"fine_angle_range"});
    number("minValidSectionRatio", "最小有效截面比例", 1, g.minValidSectionRatio, 0, 1, 3, {"min_valid_section_ratio"});
    number("maxAllowedWidthStep", "最大宽度变化 (像素)", 1, g.maxAllowedWidthStep, 0, 100000, 3, {"max_allowed_width_step"});
    number("maxCenterlineOffset", "最大中心线偏移 (像素)", 1, g.maxCenterlineOffset, 0, 100000, 3, {"max_centerline_offset"});
    number("center_search_tolerance", "中心搜索容差 (像素)", 1, g.center_search_tolerance, 0, 100000);
    number("max_consecutive_gap_samples", "最大连续缺口采样数", 2, g.maxConsecutiveGapSamples, 0, 10000, 0, {"maxConsecutiveGapSamples"});
    number("min_edge_support_density", "最小边缘支持密度", 2, g.minEdgeSupportDensity, 0, 1, 3, {"minEdgeSupportDensity"});
    number("raw_gradient_threshold", "原始梯度阈值", 2, g.rawGradientThreshold, 0, 100000, 3, {"rawGradientThreshold"});
    number("min_gradient_normal_alignment", "最小梯度法向一致性", 2, g.minGradientNormalAlignment, 0, 1, 3, {"minGradientNormalAlignment"});
    number("min_local_step_contrast", "最小局部对比度", 2, g.minLocalStepContrast, 0, 255, 3, {"minLocalStepContrast"});
    number("max_start_base_gap_px", "起点最大间距 (像素)", 2, g.maxStartBaseGapPx, 0, 100000, 3, {"maxStartBaseGapPx"});
    number("max_parallel_angle_diff_deg", "最大平行角差 (°)", 2, g.maxParallelAngleDiffDeg, 0, 180, 3, {"maxParallelAngleDiffDeg"});
    number("min_common_length_px", "最小公共长度 (像素)", 2, g.minCommonLengthPx, 0, 100000, 3, {"minCommonLengthPx"});
    number("max_line_fit_residual_px", "最大直线拟合残差 (像素)", 2, g.maxLineFitResidualPx, 0, 100000, 3, {"maxLineFitResidualPx"});
    number("max_occlusion_gap", "最大遮挡缺口 (像素)", 2, g.maxOcclusionGap, 0, 10000, 0, {"maxOcclusionGap"});
    number("corridor_half_width_px", "搜索走廊半宽 (像素)", 2, g.corridorHalfWidthPx, 0.001, 100000, 3, {"corridorHalfWidthPx"});
    flag("enable_tail_detection", "启用指针尾部检测", 3, g.enable_tail_detection);
    number("tail_max_radius", "尾部最大半径 (像素)", 3, g.tail_max_radius, 0.001, 100000);
    number("tail_min_width", "尾部最小宽度 (像素)", 3, g.tail_min_width, 0.001, 100000);
    number("tail_max_width", "尾部最大宽度 (像素)", 3, g.tail_max_width, 0.001, 100000);
    number("tail_corridor_half_width", "尾部搜索走廊半宽 (像素)", 3, g.tail_corridor_half_width, 0.001, 100000);
    number("tail_min_gradient_normal_alignment", "尾部最小梯度一致性", 3, g.tail_min_gradient_normal_alignment, 0, 1);
    flag("require_dark_pointer", "要求指针比表盘更暗", 3, g.require_dark_pointer);
    number("min_pointer_dark_contrast", "指针最小暗对比度", 3, g.min_pointer_dark_contrast, 0, 255);
    number("max_pointer_inside_gray", "指针内部最大灰度", 3, g.max_pointer_inside_gray, 0, 255);
    number("w_geom_weight", "几何结构权重", 4, g.w_geom_weight, 0, 1000, 3, {"geom_weight", "geomWeight"});
    number("w_edge_pair_coherence", "双边缘一致性权重", 4, g.w_edge_pair_coherence, 0, 1000, 3, {"edge_pair_coherence"});
    number("w_center_stability", "中心线稳定性权重", 4, g.w_center_stability, 0, 1000, 3, {"center_stability"});
    number("w_axis_center", "轴线中心权重", 4, g.w_axis_center, 0, 1000, 3, {"axis_center"});
    number("w_orientation", "方向一致性权重", 4, g.w_orientation, 0, 1000, 3, {"orientation"});
    number("w_reach", "径向延伸权重", 4, g.w_reach, 0, 1000, 3, {"reach"});
    number("w_valid_length", "有效长度权重", 4, g.w_valid_length, 0, 1000, 3, {"valid_length"});
    number("w_slenderness", "细长比权重", 4, g.w_slenderness, 0, 1000, 3, {"slenderness"});
    number("w_persistence", "持续性权重", 4, g.w_persistence, 0, 1000, 3, {"persistence"});
    number("w_width_trend", "宽度趋势权重", 4, g.w_width_trend, 0, 1000, 3, {"width_trend"});
    number("w_width_continuity", "宽度连续性权重", 4, g.w_width_continuity, 0, 1000, 3, {"width_continuity"});
    number("w_edge_strength", "边缘强度权重", 4, g.w_edge_strength, 0, 1000, 3, {"edge_strength"});
    number("w_gap_penalty", "缺口惩罚权重", 4, g.w_gap_penalty, 0, 1000, 3, {"gap_penalty"});
    number("w_tracking_jump_penalty", "追踪跳变惩罚权重", 4, g.w_tracking_jump_penalty, 0, 1000, 3, {"tracking_jump_penalty"});
    number("w_tail_score", "尾部评分权重", 4, g.w_tail_score, 0, 1000, 3, {"tail_score"});
    number("w_tail_polarity_boost", "尾部极性增强权重", 4, g.w_tail_polarity_boost, 0, 1000, 3, {"tail_polarity_boost"});
    number("confidence_divisor", "置信度归一化系数", 4, g.confidence_divisor, 0.001, 100000);
    for (auto& field : result) {
        if (field.page < 2 || field.key == "w_geom_weight" || !field.key.contains('_')) continue;
        const auto words = field.key.split('_');
        QString camel = words.first();
        for (int i = 1; i < words.size(); ++i) {
            auto word = words[i];
            if (!word.isEmpty()) word[0] = word[0].toUpper();
            camel += word;
        }
        if (!field.aliases.contains(camel)) field.aliases.append(camel);
    }
    return result;
}

QJsonObject GaugeTemplateStore::defaults() {
    QJsonObject result{{"gauge_id", ""}, {"unit", "MPa"}};
    for (const auto& field : fields()) result[field.key] = field.defaultValue;
    return result;
}

QString GaugeTemplateStore::validate(const QJsonObject& profile) {
    if (profile["gauge_id"].toString().isEmpty()) return QStringLiteral(u"数据库模板必须填写模板编号");
    for (const auto& key : {"use_model", "gauge_profile", "profile_model", "parent_model_path", "default_profile_id"})
        if (!profile[key].toString().isEmpty()) return QStringLiteral(u"数据库模板不能引用本地模型文件，请先迁移为独立模板");
    const auto id = idOf(profile);
    if (id.trimmed().isEmpty() || id != id.trimmed() || id.size() > 128)
        return QStringLiteral(u"模板编号不能为空、含首尾空格或超过 128 个字符");
    for (const auto ch : id) if ((ch.isSpace() && ch != QChar(' ')) || ch == '"' || ch == '\\' || ch.unicode() < 32)
        return QStringLiteral(u"模板编号不能包含换行、引号或反斜杠");
    if (profile["unit"].toString().trimmed().isEmpty()) return QStringLiteral(u"请填写读数单位");
    auto values = defaults();
    for (const auto& field : fields()) {
        QJsonValue value = profile[field.key];
        if (value.isUndefined()) for (const auto& alias : field.aliases)
            if (profile.contains(alias)) { value = profile[alias]; break; }
        if (value.isUndefined()) continue;
        if (field.defaultValue.isBool()) {
            if (!value.isBool()) return QStringLiteral(u"%1 必须为开关值").arg(field.label);
        } else if (!value.isDouble() || !std::isfinite(value.toDouble()) || value.toDouble() < field.minimum ||
                   value.toDouble() > field.maximum || (field.decimals == 0 && std::floor(value.toDouble()) != value.toDouble()))
            return QStringLiteral(u"%1 超出有效范围").arg(field.label);
        values[field.key] = value;
    }
    if (values["max_val"].toDouble() <= values["min_val"].toDouble()) return QStringLiteral(u"量程上限必须大于下限");
    if (values["pointerMaxWidth"].toDouble() < values["pointerMinWidth"].toDouble()) return QStringLiteral(u"指针最大宽度不能小于最小宽度");
    if (values["tail_max_width"].toDouble() < values["tail_min_width"].toDouble()) return QStringLiteral(u"尾部最大宽度不能小于最小宽度");
    return {};
}

bool GaugeTemplateStore::saveProfile(const QJsonObject& profile, bool replacing, QString& error) {
    error = validate(profile);
    if (!error.isEmpty()) return false;
    if (!m_loaded) { error = QStringLiteral(u"数据库模板库尚未成功加载"); return false; }
    const auto id = idOf(profile);
    if (m_records.contains(id) != replacing) {
        error = replacing ? QStringLiteral(u"该模板已不存在，请刷新列表") : QStringLiteral(u"模板编号已存在，请使用不同编号");
        return false;
    }
    try {
        auto cleaned=profile; cleaned.remove("min_angle_deg"); cleaned.remove("max_angle_deg");
        auto record = m_backend.save(cleaned, replacing ? m_records.value(id).revision : 0);
        record.usedBy = m_records.value(id).usedBy;
        m_records[id] = record;
        error.clear(); return true;
    } catch (const std::exception& e) { error = QString::fromUtf8(e.what()); return false; }
}

bool GaugeTemplateStore::removeProfile(const QString& id, const QStringList& usedBy, QString& error) {
    error.clear();
    if (!usedBy.isEmpty()) { error = QStringLiteral(u"以下设备正在使用此模板，请先在设备档案中更换模板：\n%1").arg(usedBy.join("\n")); return false; }
    if (!m_loaded || !m_records.contains(id)) { error = QStringLiteral(u"数据库模板库尚未加载或模板已不存在"); return false; }
    try {
        m_backend.remove(id, m_records.value(id).revision);
        m_records.remove(id);
        return true;
    } catch (const std::exception& e) { error = QString::fromUtf8(e.what()); return false; }
}

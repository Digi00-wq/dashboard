// Swift Charts building blocks and overview cards of the History tab (steps, sleep, heart, energy).

import Charts
import SwiftUI

// MARK: - Shared pieces

/// "Average" caption + big number, like the Health app.
struct HistoryHeadline: View {
    let caption: LocalizedStringKey
    let value: Text

    var body: some View {
        VStack(alignment: .leading, spacing: 2) {
            Text(caption)
                .font(.caption.weight(.medium))
                .foregroundStyle(.secondary)
                .textCase(.uppercase)
            value
                .font(.title2.weight(.semibold).monospacedDigit())
        }
    }
}

private struct RangeXAxis: ViewModifier {
    let range: HistoryRange

    func body(content: Content) -> some View {
        content
            .chartXScale(domain: range.domain)
            .chartXAxis {
                AxisMarks(values: .stride(by: range.axisStride, count: range.axisCount)) { _ in
                    AxisGridLine()
                    AxisValueLabel(format: range.axisFormat, centered: range.axisCentered)
                }
            }
    }
}

extension View {
    func rangeXAxis(_ range: HistoryRange) -> some View { modifier(RangeXAxis(range: range)) }
}

// MARK: - Bars (steps, energy)

struct HistoryBarChart: View {
    let points: [HistoryChartPoint]
    let range: HistoryRange
    let label: String
    let color: Color
    let average: Double?
    var height: CGFloat = 160

    var body: some View {
        Chart {
            ForEach(points) { p in
                BarMark(x: .value("Date", p.date, unit: range.unit),
                        y: .value(label, p.value))
                    .foregroundStyle(p.isCurrent ? color : color.opacity(0.55))
                    .cornerRadius(range == .week ? 4 : 2)
            }
            if let average {
                RuleMark(y: .value("Average", average))
                    .foregroundStyle(Color.secondary)
                    .lineStyle(StrokeStyle(lineWidth: 1, dash: [4, 3]))
            }
        }
        .rangeXAxis(range)
        .frame(height: height)
    }
}

// MARK: - Sleep (stacked stages)

struct HistorySleepChart: View {
    let points: [HistoryStagePoint]
    let range: HistoryRange
    let averageHours: Double?
    var height: CGFloat = 160
    var showLegend = true

    var body: some View {
        Chart {
            ForEach(points) { p in
                BarMark(x: .value("Night", p.date, unit: range.unit),
                        y: .value("Hours", p.minutes / 60))
                    .foregroundStyle(by: .value("Stage", p.stage.label))
            }
            if let averageHours {
                RuleMark(y: .value("Average", averageHours))
                    .foregroundStyle(Color.secondary)
                    .lineStyle(StrokeStyle(lineWidth: 1, dash: [4, 3]))
            }
        }
        .chartForegroundStyleScale([
            HistorySleepStage.deep.label: HistorySleepStage.deep.color,
            HistorySleepStage.core.label: HistorySleepStage.core.color,
            HistorySleepStage.rem.label: HistorySleepStage.rem.color,
            HistorySleepStage.awake.label: HistorySleepStage.awake.color,
        ])
        .chartLegend(showLegend ? .visible : .hidden)
        .chartYAxis {
            AxisMarks { value in
                AxisGridLine()
                AxisValueLabel {
                    if let h = value.as(Double.self) { Text("\(Int(h)) h") }
                }
            }
        }
        .rangeXAxis(range)
        .frame(height: height)
    }
}

// MARK: - Lines (resting HR, HRV)

struct HistoryLineChart: View {
    let points: [HistoryChartPoint]
    let range: HistoryRange
    let label: String
    let color: Color
    var height: CGFloat = 110

    var body: some View {
        Chart {
            ForEach(points) { p in
                LineMark(x: .value("Date", p.date, unit: range.unit),
                         y: .value(label, p.value))
                    .foregroundStyle(color)
                    .interpolationMethod(.catmullRom)
                PointMark(x: .value("Date", p.date, unit: range.unit),
                          y: .value(label, p.value))
                    .foregroundStyle(color)
                    .symbolSize(range == .week ? 30 : 12)
            }
        }
        .chartYScale(domain: .automatic(includesZero: false))
        .rangeXAxis(range)
        .frame(height: height)
    }
}

// MARK: - Overview cards

/// Chevron shown in the card header: the card opens a detail screen.
private let chevron = AnyView(
    Image(systemName: "chevron.right")
        .font(.footnote.weight(.semibold))
        .foregroundStyle(.tertiary)
)

struct HistoryStepsCard: View {
    let days: [HistoryDailyEntry]
    let range: HistoryRange

    var body: some View {
        let avg = HistoryAggregate.average(days) { $0.steps }
        Card(title: "Steps", systemImage: "figure.walk", tint: .orange, trailing: chevron) {
            if let avg {
                HistoryHeadline(caption: "Daily average", value: Text("\(Fmt.number(Int(avg.rounded()))) steps"))
                HistoryBarChart(points: HistoryAggregate.series(days, weekly: range.weekly) { $0.steps },
                               range: range, label: String(localized: "Steps"), color: .orange, average: avg)
            } else {
                HistoryNoData()
            }
        }
    }
}

struct HistorySleepCard: View {
    let days: [HistoryDailyEntry]
    let range: HistoryRange

    var body: some View {
        let avg = HistoryAggregate.average(days) { $0.asleepMin }
        Card(title: "Sleep", systemImage: "bed.double.fill", tint: .indigo, trailing: chevron) {
            if let avg {
                HistoryHeadline(caption: "Average time asleep", value: Text(Fmt.minutes(Int(avg.rounded()))))
                HistorySleepChart(points: HistoryAggregate.sleepStages(days, weekly: range.weekly),
                                 range: range, averageHours: avg / 60)
            } else {
                HistoryNoData()
            }
        }
    }
}

struct HistoryHeartCard: View {
    let days: [HistoryDailyEntry]
    let range: HistoryRange

    var body: some View {
        let resting = HistoryAggregate.average(days) { $0.restingHr }
        let hrv = HistoryAggregate.average(days) { $0.hrvMs }
        Card(title: "Heart", systemImage: "heart.fill", tint: .pink, trailing: chevron) {
            if resting == nil && hrv == nil {
                HistoryNoData()
            }
            if let resting {
                HistoryHeadline(caption: "Resting heart rate", value: Text("\(Int(resting.rounded())) bpm"))
                HistoryLineChart(points: HistoryAggregate.series(days, weekly: range.weekly) { $0.restingHr },
                                range: range, label: String(localized: "Resting HR"), color: .pink)
            }
            if let hrv {
                HistoryHeadline(caption: "Heart rate variability", value: Text("\(Int(hrv.rounded())) ms"))
                HistoryLineChart(points: HistoryAggregate.series(days, weekly: range.weekly) { $0.hrvMs },
                                range: range, label: String(localized: "HRV"), color: .purple)
            }
        }
    }
}

struct HistoryEnergyCard: View {
    let days: [HistoryDailyEntry]
    let range: HistoryRange

    var body: some View {
        let avg = HistoryAggregate.average(days) { $0.activeKcal }
        let total = HistoryAggregate.total(days) { $0.activeKcal }
        Card(title: "Active energy", systemImage: "flame.fill", tint: .red, trailing: chevron) {
            if let avg {
                HStack(alignment: .top, spacing: 24) {
                    HistoryHeadline(caption: "Daily average", value: Text("\(Fmt.number(Int(avg.rounded()))) kcal"))
                    HistoryHeadline(caption: "Total", value: Text("\(Fmt.number(Int(total.rounded()))) kcal"))
                }
                HistoryBarChart(points: HistoryAggregate.series(days, weekly: range.weekly) { $0.activeKcal },
                               range: range, label: String(localized: "Active energy"), color: .red, average: avg)
            } else {
                HistoryNoData()
            }
        }
    }
}

struct HistoryNoData: View {
    var body: some View {
        Text("No data in this period")
            .font(.subheadline)
            .foregroundStyle(.secondary)
            .frame(maxWidth: .infinity, minHeight: 60)
    }
}

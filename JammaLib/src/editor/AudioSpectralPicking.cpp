#include "AudioSpectralPicking.h"

#include <algorithm>
#include <cmath>
#include <limits>
#include <initializer_list>
#include <utility>

using namespace editor;

bool AudioSpectralPicking::_Finite(glm::dvec3 value) noexcept
{
	return std::isfinite(value.x) && std::isfinite(value.y) && std::isfinite(value.z);
}

bool AudioSpectralPicking::_Slab(double origin, double direction, double minimum,
	double maximum, double& first, double& last) noexcept
{
	if (direction == 0.0) return origin >= minimum && origin <= maximum;
	auto nearDistance = (minimum - origin) / direction;
	auto farDistance = (maximum - origin) / direction;
	if (nearDistance > farDistance) std::swap(nearDistance, farDistance);
	first = (std::max)(first, nearDistance);
	last = (std::min)(last, farDistance);
	return first <= last;
}

std::optional<AudioSpectralPicking::Hit> AudioSpectralPicking::_Triangle(
	glm::dvec3 origin, glm::dvec3 direction, glm::dvec3 a, glm::dvec3 b, glm::dvec3 c,
	glm::dvec2 uvA, glm::dvec2 uvB, glm::dvec2 uvC,
	double first, double last, double positionTolerance) noexcept
{
	if (!_Finite(a) || !_Finite(b) || !_Finite(c)) return std::nullopt;
	const auto edge1 = b - a;
	const auto edge2 = c - a;
	const auto normal = glm::cross(edge1, edge2);
	const auto normalSquared = glm::dot(normal, normal);
	if (!(normalSquared > 0.0) || !std::isfinite(normalSquared)) return std::nullopt;
	const auto relative = origin - a;
	const auto planeOrigin = glm::dot(normal, relative);
	const auto planeDirection = glm::dot(normal, direction);
	const auto normalLength = std::sqrt(normalSquared);
	const auto bary1 = glm::dot(glm::cross(relative, edge2), normal) / normalSquared;
	const auto bary2 = glm::dot(glm::cross(edge1, relative), normal) / normalSquared;
	const auto slope1 = glm::dot(glm::cross(direction, edge2), normal) / normalSquared;
	const auto slope2 = glm::dot(glm::cross(edge1, direction), normal) / normalSquared;
	constexpr double baryTolerance = 1e-9;
	double distance;
	if (std::abs(planeDirection) > normalLength * 1e-12)
	{
		distance = -planeOrigin / planeDirection;
		if (distance < first - positionTolerance || distance > last + positionTolerance)
			return std::nullopt;
		distance = std::clamp(distance, first, last);
	}
	else
	{
		if (std::abs(planeOrigin) > normalLength * positionTolerance) return std::nullopt;
		// A coplanar grazing ray has an interval of triangle hits. Clip all three
		// affine barycentric coordinates to get its nearest forward entry.
		auto nearDistance = first;
		auto farDistance = last;
		const auto clip = [&nearDistance, &farDistance](double value, double slope) noexcept
		{
			if (slope == 0.0) return value >= -baryTolerance;
			const auto crossing = (-baryTolerance - value) / slope;
			if (slope > 0.0) nearDistance = (std::max)(nearDistance, crossing);
			else farDistance = (std::min)(farDistance, crossing);
			return nearDistance <= farDistance;
		};
		if (!clip(bary1, slope1) || !clip(bary2, slope2)
			|| !clip(1.0 - bary1 - bary2, -slope1 - slope2)) return std::nullopt;
		distance = nearDistance;
	}
	auto weight1 = bary1 + distance * slope1;
	auto weight2 = bary2 + distance * slope2;
	auto weight0 = 1.0 - weight1 - weight2;
	if (weight0 < -baryTolerance || weight1 < -baryTolerance || weight2 < -baryTolerance)
		return std::nullopt;
	weight0 = (std::max)(weight0, 0.0);
	weight1 = (std::max)(weight1, 0.0);
	weight2 = (std::max)(weight2, 0.0);
	const auto uv = (uvA * weight0 + uvB * weight1 + uvC * weight2) / (weight0 + weight1 + weight2);
	return Hit{ std::clamp(uv.x, 0.0, 1.0), std::clamp(uv.y, 0.0, 1.0), distance };
}

std::optional<AudioSpectralPicking::Hit> AudioSpectralPicking::Intersect(
	const graphics::AudioSpectralModel::Surface& surface,
	glm::vec3 localOrigin, glm::vec3 localDirection) noexcept
{
	const auto display = surface.Display.get();
	// IsValid() scans every height; the model validates once on publication.
	// Check constant-size metadata here, and only inspect visited cell vertices.
	if (!display || surface.Morph != 1.0f || !std::isfinite(surface.Radius) || surface.Radius <= 0.0f || !std::isfinite(surface.PlayFraction)
		|| !display->SourceIdentity || display->SourceGeneration == 0u || display->Revision == 0u
		|| display->TimeSegments == 0u || display->FrequencySegments == 0u
		|| display->TimeSegments > audio::AudioSpectralConfig::MaxTimeSegments
		|| display->FrequencySegments > audio::AudioSpectralConfig::MaxFrequencySegments
		|| !std::isfinite(display->MinimumFrequencyHz) || !(display->MinimumFrequencyHz > 0.0f)
		|| !std::isfinite(display->NyquistHz) || !(display->NyquistHz > display->MinimumFrequencyHz)
		|| display->Heights.size() != static_cast<std::size_t>(display->TimeSegments + 1u) * (display->FrequencySegments + 1u))
		return std::nullopt;
	const glm::dvec3 origin(localOrigin);
	glm::dvec3 direction(localDirection);
	if (!_Finite(origin) || !_Finite(direction)) return std::nullopt;
	const auto directionLength = glm::length(direction);
	if (!(directionLength > 0.0) || !std::isfinite(directionLength)) return std::nullopt;
	direction /= directionLength;
	const auto columns = display->TimeSegments;
	const auto rows = display->FrequencySegments;
	const auto corner = graphics::AudioSpectralModel::LocalVertex(surface, 0u, 0u);
	const auto opposite = graphics::AudioSpectralModel::LocalVertex(surface, columns, rows);
	const auto minimumX = static_cast<double>(corner.x);
	const auto maximumX = static_cast<double>(opposite.x);
	const auto minimumZ = static_cast<double>(opposite.z);
	const auto maximumZ = static_cast<double>(corner.z);
	const auto maximumY = static_cast<double>(graphics::AudioSpectralModel::SurfaceBaseY
		+ surface.Radius * graphics::AudioSpectralModel::HeightRadiusScale);
	const auto tolerance = (std::max)(1.0, static_cast<double>(surface.Radius)) * 1e-7;
	double first = 0.0;
	double last = std::numeric_limits<double>::infinity();
	if (!(maximumX > minimumX) || !(maximumZ > minimumZ) || !std::isfinite(maximumY)
		|| !_Slab(origin.x, direction.x, minimumX, maximumX, first, last)
		|| !_Slab(origin.z, direction.z, minimumZ, maximumZ, first, last)
		|| !_Slab(origin.y, direction.y, graphics::AudioSpectralModel::SurfaceBaseY, maximumY, first, last))
		return std::nullopt;
	std::optional<Hit> best;
	const auto testCell = [&](int column, int row) noexcept
	{
		if (column < 0 || row < 0 || column >= static_cast<int>(columns) || row >= static_cast<int>(rows)) return;
		const auto x = static_cast<unsigned int>(column);
		const auto z = static_cast<unsigned int>(row);
		const auto stride = columns + 1u;
		for (const auto index : { z * stride + x, z * stride + x + 1u,
			(z + 1u) * stride + x, (z + 1u) * stride + x + 1u })
			if (!std::isfinite(display->Heights[index]) || display->Heights[index] < 0.0f || display->Heights[index] > 1.0f) return;
		const glm::dvec3 a(graphics::AudioSpectralModel::LocalVertex(surface, x, z));
		const glm::dvec3 b(graphics::AudioSpectralModel::LocalVertex(surface, x + 1u, z));
		const glm::dvec3 c(graphics::AudioSpectralModel::LocalVertex(surface, x, z + 1u));
		const glm::dvec3 d(graphics::AudioSpectralModel::LocalVertex(surface, x + 1u, z + 1u));
		const auto uvA = glm::dvec2(static_cast<float>(x) / columns, static_cast<float>(z) / rows);
		const auto uvB = glm::dvec2(static_cast<float>(x + 1u) / columns, static_cast<float>(z) / rows);
		const auto uvC = glm::dvec2(static_cast<float>(x) / columns, static_cast<float>(z + 1u) / rows);
		const auto uvD = glm::dvec2(static_cast<float>(x + 1u) / columns, static_cast<float>(z + 1u) / rows);
		const auto accept = [&best](const std::optional<Hit>& hit) noexcept
		{
			if (hit && (!best || hit->Distance < best->Distance)) best = hit;
		};
		accept(_Triangle(origin, direction, a, b, c, uvA, uvB, uvC, first, last, tolerance));
		accept(_Triangle(origin, direction, b, d, c, uvB, uvD, uvC, first, last, tolerance));
	};
	const auto testPointCells = [&](double distance) noexcept
	{
		const auto point = origin + direction * distance;
		const auto u = std::clamp((point.x - minimumX) / (maximumX - minimumX), 0.0, 1.0) * columns;
		const auto v = std::clamp((maximumZ - point.z) / (maximumZ - minimumZ), 0.0, 1.0) * rows;
		const auto column = (std::min)(static_cast<int>(std::floor(u)), static_cast<int>(columns) - 1);
		const auto row = (std::min)(static_cast<int>(std::floor(v)), static_cast<int>(rows) - 1);
		testCell(column, row);
		const auto nearestColumn = static_cast<int>(std::round(u));
		const auto nearestRow = static_cast<int>(std::round(v));
		const bool onX = std::abs(point.x - graphics::AudioSpectralModel::LocalVertex(surface, nearestColumn, 0u).x) <= tolerance;
		const bool onZ = std::abs(point.z - graphics::AudioSpectralModel::LocalVertex(surface, 0u, nearestRow).z) <= tolerance;
		if (onX) { testCell(nearestColumn - 1, row); testCell(nearestColumn, row); }
		if (onZ) { testCell(column, nearestRow - 1); testCell(column, nearestRow); }
		if (onX && onZ)
			for (int dx = -1; dx <= 0; ++dx)
				for (int dz = -1; dz <= 0; ++dz) testCell(nearestColumn + dx, nearestRow + dz);
	};
	testPointCells(first);
	if (direction.x == 0.0 && direction.z == 0.0) return best;
	const auto point = origin + direction * first;
	int column = (std::min)(static_cast<int>(std::floor(std::clamp((point.x - minimumX) / (maximumX - minimumX), 0.0, 1.0) * columns)), static_cast<int>(columns) - 1);
	int row = (std::min)(static_cast<int>(std::floor(std::clamp((maximumZ - point.z) / (maximumZ - minimumZ), 0.0, 1.0) * rows)), static_cast<int>(rows) - 1);
	const int stepX = direction.x > 0.0 ? 1 : direction.x < 0.0 ? -1 : 0;
	const int stepZ = direction.z < 0.0 ? 1 : direction.z > 0.0 ? -1 : 0;
	auto current = first;
	// Each iteration advances at least one grid axis. Corner visits explicitly
	// check both neighbours; traversal work is O(timeSegments+frequencySegments).
	for (unsigned int iteration = 0u; iteration <= columns + rows + 2u; ++iteration)
	{
		if (column < 0 || row < 0 || column >= static_cast<int>(columns) || row >= static_cast<int>(rows)) break;
		const auto nextX = stepX == 0 ? std::numeric_limits<double>::infinity()
			: (graphics::AudioSpectralModel::LocalVertex(surface, column + (stepX > 0 ? 1 : 0), 0u).x - origin.x) / direction.x;
		const auto nextZ = stepZ == 0 ? std::numeric_limits<double>::infinity()
			: (graphics::AudioSpectralModel::LocalVertex(surface, 0u, row + (stepZ > 0 ? 1 : 0)).z - origin.z) / direction.z;
		const auto next = (std::min)(last, (std::min)(nextX, nextZ));
		testCell(column, row);
		if (next > current) testPointCells(current + (next - current) * 0.5);
		testPointCells((std::max)(current, next));
		if (best && best->Distance <= next + tolerance) return best;
		if (next >= last) break;
		// An exact corner advances both axes; nearDistance corners are handled by the
		// neighbour checks without merging distinct crossing parameters.
		if (nextX <= nextZ) column += stepX;
		if (nextZ <= nextX) row += stepZ;
		current = (std::max)(current, next);
	}
	return best;
}

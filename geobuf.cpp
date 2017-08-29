#include <stdio.h>
#include <string>
#include "mvt.hpp"
#include "serial.hpp"
#include "geobuf.hpp"
#include "geojson.hpp"
#include "projection.hpp"
#include "protozero/varint.hpp"
#include "protozero/pbf_reader.hpp"
#include "protozero/pbf_writer.hpp"
#include "milo/dtoa_milo.h"

#define POINT 0
#define MULTIPOINT 1
#define LINESTRING 2
#define MULTILINESTRING 3
#define POLYGON 4
#define MULTIPOLYGON 5

serial_val readValue(protozero::pbf_reader &pbf, std::vector<std::string> &keys) {
	serial_val sv;
	sv.type = mvt_null;
	sv.s = "null";

	while (pbf.next()) {
		switch (pbf.tag()) {
		case 1:
			sv.type = mvt_string;
			sv.s = pbf.get_string();
			break;

		case 2:
			sv.type = mvt_double;
			sv.s = milo::dtoa_milo(pbf.get_double());
			break;

		case 3:
			sv.type = mvt_double;
			sv.s = std::to_string(pbf.get_uint64());
			break;

		case 4:
			sv.type = mvt_double;
			sv.s = std::to_string(-pbf.get_uint64());
			break;

		case 5:
			sv.type = mvt_bool;
			if (pbf.get_bool()) {
				sv.s = "true";
			} else {
				sv.s = "false";
			}
			break;

		case 6:
			sv.type = mvt_string;  // stringified JSON
			sv.s = pbf.get_string();
			break;

		default:
			pbf.skip();
		}
	}

	return sv;
}

drawvec readPoint(std::vector<long long> &coords, std::vector<int> &lengths, size_t dim, double e) {
	long long x, y;
	projection->project(coords[0] / e, coords[1] / e, 32, &x, &y);
	drawvec dv;
	dv.push_back(draw(VT_MOVETO, x, y));
	return dv;
}

drawvec readLinePart(std::vector<long long> &coords, std::vector<int> &lengths, size_t dim, double e, size_t start, size_t end, bool closed) {
	drawvec dv;
	std::vector<long long> prev;
	std::vector<double> p;
	prev.resize(dim);
	p.resize(dim);

	for (size_t i = start; i + dim - 1 < end; i += dim) {
		if (i + dim - 1 >= coords.size()) {
			fprintf(stderr, "Internal error: line segment %zu vs %zu\n", i + dim - 1, coords.size());
			exit(EXIT_FAILURE);
		}

		for (size_t d = 0; d < dim; d++) {
			prev[d] += coords[i + d];
			p[d] = prev[d] / e;
		}

		long long x, y;
		projection->project(p[0], p[1], 32, &x, &y);

		if (i == start) {
			dv.push_back(draw(VT_MOVETO, x, y));
		} else {
			dv.push_back(draw(VT_LINETO, x, y));
		}
	}

	if (closed && dv.size() > 0) {
		dv.push_back(draw(VT_LINETO, dv[0].x, dv[0].y));
	}

	return dv;
}

drawvec readLine(std::vector<long long> &coords, std::vector<int> &lengths, size_t dim, double e, bool closed) {
	return readLinePart(coords, lengths, dim, e, 0, coords.size(), closed);
}

drawvec readMultiLine(std::vector<long long> &coords, std::vector<int> &lengths, size_t dim, double e, bool closed) {
	if (lengths.size() == 0) {
		return readLinePart(coords, lengths, dim, e, 0, coords.size(), closed);
	}

	drawvec dv;
	size_t here = 0;
	for (size_t i = 0; i < lengths.size(); i++) {
		drawvec dv2 = readLinePart(coords, lengths, dim, e, here, here + lengths[i] * dim, closed);
		here += lengths[i] * dim;

		for (size_t j = 0; j < dv2.size(); j++) {
			dv.push_back(dv2[j]);
		}
	}

	return dv;
}

drawvec readMultiPolygon(std::vector<long long> &coords, std::vector<int> &lengths, size_t dim, double e) {
	if (lengths.size() == 0) {
		return readLinePart(coords, lengths, dim, e, 0, coords.size(), true);
	}

	size_t polys = lengths[0];
	size_t n = 1;
	size_t here = 0;
	drawvec dv;

	for (size_t i = 0; i < polys; i++) {
		size_t rings = lengths[n++];

		for (size_t j = 0; j < rings; j++) {
			drawvec dv2 = readLinePart(coords, lengths, dim, e, here, here + lengths[n] * dim, true);
			here += lengths[n] * dim;
			n++;

			for (size_t k = 0; k < dv2.size(); k++) {
				dv.push_back(dv2[k]);
			}
		}

		dv.push_back(draw(VT_CLOSEPATH, 0, 0));  // mark that the next ring is outer
	}

	return dv;
}

drawvec readGeometry(protozero::pbf_reader &pbf, size_t dim, double e, std::vector<std::string> &keys, int &type) {
	std::vector<long long> coords;
	std::vector<int> lengths;

	while (pbf.next()) {
		switch (pbf.tag()) {
		case 1:
			type = pbf.get_enum();
			break;

		case 2: {
			auto pi = pbf.get_packed_uint32();
			for (auto it = pi.first; it != pi.second; ++it) {
				lengths.push_back(*it);
			}
			break;
		}

		case 3: {
			auto pi = pbf.get_packed_sint64();
			for (auto it = pi.first; it != pi.second; ++it) {
				coords.push_back(*it);
			}
			break;
		}

		case 4: {
			int type2;
			protozero::pbf_reader geometry_reader(pbf.get_message());
			drawvec dv2 = readGeometry(geometry_reader, dim, e, keys, type2);
			break;
		}

		default:
			pbf.skip();
		}
	}

	drawvec dv;
	if (type == POINT) {
		dv = readPoint(coords, lengths, dim, e);
	} else if (type == MULTIPOINT) {
		dv = readLine(coords, lengths, dim, e, true);
	} else if (type == LINESTRING) {
		dv = readLine(coords, lengths, dim, e, false);
	} else if (type == POLYGON) {
		dv = readMultiLine(coords, lengths, dim, e, true);
	} else if (type == MULTIPOLYGON) {
		dv = readMultiPolygon(coords, lengths, dim, e);
	}

	type = type / 2 + 1;
	return dv;
}

void readFeature(protozero::pbf_reader &pbf, size_t dim, double e, std::vector<std::string> &keys, struct serialization_state *sst, int layer, std::string layername) {
	drawvec dv;
	long long id = 0;
	bool has_id = false;
	std::vector<serial_val> values;
	std::vector<size_t> properties;
	int type = 0;

	while (pbf.next()) {
		switch (pbf.tag()) {
		case 1: {
			protozero::pbf_reader geometry_reader(pbf.get_message());
			dv = readGeometry(geometry_reader, dim, e, keys, type);
			break;
		}

		case 11: {
			static bool warned = false;
			if (!warned) {
				fprintf(stderr, "Non-numeric feature IDs not supported\n");
				warned = true;
			}
			pbf.skip();
			break;
		}

		case 12:
			id = pbf.get_int64();
			has_id = true;
			break;

		case 13: {
			protozero::pbf_reader value_reader(pbf.get_message());
			values.push_back(readValue(value_reader, keys));
			break;
		}

		case 14: {
			auto pi = pbf.get_packed_uint32();
			for (auto it = pi.first; it != pi.second; ++it) {
				properties.push_back(*it);
			}
			break;
		}

		default:
			pbf.skip();
		}
	}

	serial_feature sf;
	sf.layer = layer;
	sf.layername = layername;
	sf.segment = 0;  // single thread
	sf.has_id = has_id;
	sf.id = id;
	sf.has_tippecanoe_minzoom = false;
	sf.has_tippecanoe_maxzoom = false;
	sf.feature_minzoom = false;
	sf.seq = (*sst->layer_seq);
	sf.geometry = dv;
	sf.t = type;

	for (size_t i = 0; i + 1 < properties.size(); i += 2) {
		if (properties[i] >= keys.size()) {
			fprintf(stderr, "Out of bounds key: %zu in %zu\n", properties[i], keys.size());
			exit(EXIT_FAILURE);
		}

		if (properties[i + 1] >= values.size()) {
			fprintf(stderr, "Out of bounds value: %zu in %zu\n", properties[i + 1], values.size());
			exit(EXIT_FAILURE);
		}

		sf.full_keys.push_back(keys[properties[i]]);
		sf.full_values.push_back(values[properties[i + 1]]);
	}

	sf.m = sf.full_values.size();

	serialize_feature(sst, sf);
}

void readFeatureCollection(protozero::pbf_reader &pbf, size_t dim, double e, std::vector<std::string> &keys, struct serialization_state *sst, int layer, std::string layername) {
	while (pbf.next()) {
		switch (pbf.tag()) {
		case 1: {
			protozero::pbf_reader feature_reader(pbf.get_message());
			readFeature(feature_reader, dim, e, keys, sst, layer, layername);
			break;
		}

		default:
			pbf.skip();
		}
	}
}

void parse_geobuf(struct serialization_state *sst, const char *src, size_t len, int layer, std::string layername) {
	protozero::pbf_reader pbf(src, len);

	size_t dim = 2;
	double e = 1e6;
	std::vector<std::string> keys;

	while (pbf.next()) {
		switch (pbf.tag()) {
		case 1:
			keys.push_back(pbf.get_string());
			break;

		case 2:
			dim = pbf.get_int64();
			if (dim < 2) {
				fprintf(stderr, "Geometry has fewer than 2 dimensions: %zu\n", dim);
				exit(EXIT_FAILURE);
			}
			break;

		case 3:
			e = pow(10, pbf.get_int64());
			break;

		case 4: {
			protozero::pbf_reader feature_collection_reader(pbf.get_message());
			readFeatureCollection(feature_collection_reader, dim, e, keys, sst, layer, layername);
			break;
		}

		case 5: {
			protozero::pbf_reader feature_reader(pbf.get_message());
			readFeature(feature_reader, dim, e, keys, sst, layer, layername);
			break;
		}

		case 6: {
			int type;
			protozero::pbf_reader geometry_reader(pbf.get_message());
			drawvec dv = readGeometry(geometry_reader, dim, e, keys, type);
			break;
		}

		default:
			pbf.skip();
		}
	}
}

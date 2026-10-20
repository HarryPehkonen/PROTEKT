#include "protekt/app/render.hpp"

#include <cstdint>
#include <fstream>
#include <memory>
#include <ostream>
#include <string>
#include <vector>

#include "protekt/app/args.hpp"
#include "protekt/game/atlas.hpp"
#include "protekt/render/renderer.hpp"

namespace protekt::app {

namespace {

struct Options {
    std::uint64_t seed{42};
    std::uint32_t width{640};
    std::uint32_t height{400};
    std::string out{"frame.ppm"};
    std::string gpu;
    bool list_gpus{false};
};

const char* kind_of(const render::DeviceInfo& d) {
    return d.discrete ? "discrete" : d.integrated ? "integrated" : "cpu";
}

} // namespace

int run_render_cli(std::span<const std::string_view> args, std::ostream& out, std::ostream& err) {
    Options opt;
    for (std::size_t i = 0; i < args.size(); ++i) {
        const std::string_view arg = args[i];
        const auto value = [&](std::string_view& text) {
            if (i + 1 >= args.size()) {
                err << "protekt-render: " << arg << " needs a value\n";
                return false;
            }
            text = args[++i];
            return true;
        };
        std::string_view text;
        bool ok = true;
        if (arg == "--list-gpus") {
            opt.list_gpus = true;
        } else if (arg == "--seed") {
            ok = value(text);
            if (ok && !parse_number(text, opt.seed)) {
                err << "protekt-render: --seed wants an unsigned integer, got '" << text << "'\n";
                ok = false;
            }
        } else if (arg == "--width") {
            ok = value(text);
            if (ok && (!parse_number(text, opt.width) || opt.width == 0)) {
                err << "protekt-render: --width wants a positive integer, got '" << text << "'\n";
                ok = false;
            }
        } else if (arg == "--height") {
            ok = value(text);
            if (ok && (!parse_number(text, opt.height) || opt.height == 0)) {
                err << "protekt-render: --height wants a positive integer, got '" << text << "'\n";
                ok = false;
            }
        } else if (arg == "--out") {
            ok = value(text);
            opt.out = std::string(text);
        } else if (arg == "--gpu") {
            ok = value(text);
            opt.gpu = std::string(text);
        } else {
            err << "protekt-render: unknown argument '" << arg << "'\n";
            ok = false;
        }
        if (!ok) {
            return 2;
        }
    }

    std::string error;
    if (opt.list_gpus) {
        const auto list = render::devices(error);
        if (list.empty()) {
            err << "protekt-render: no Vulkan device: " << error << '\n';
            return 4;
        }
        for (const auto& d : list) {
            out << d.name << " (" << kind_of(d) << ")\n";
        }
        return 0;
    }

    render::CreateOptions create_options;
    create_options.device_name = opt.gpu;
    create_options.extent = game::Extent{opt.width, opt.height};
    create_options.atlas_rgba = game::atlas_pixels();
    create_options.atlas_side = game::kAtlasSide;
    const auto renderer = render::create(create_options, error);
    if (!renderer) {
        err << "protekt-render: cannot create a renderer: " << error << '\n';
        return 4;
    }

    render::FrameStats stats{};
    const auto rgba = renderer->render(
        render::FrameRequest{.extent = create_options.extent, .seed = opt.seed, .tick = 0}, stats);
    const std::size_t texels = static_cast<std::size_t>(opt.width) * opt.height;
    if (rgba.size() != texels * 4U) {
        err << "protekt-render: the renderer returned no frame\n";
        return 1;
    }

    std::vector<char> rgb;
    rgb.reserve(texels * 3U);
    for (std::size_t i = 0; i < texels; ++i) {
        for (std::size_t c = 0; c < 3; ++c) {
            rgb.push_back(static_cast<char>(rgba[i * 4U + c]));
        }
    }
    std::ofstream file(opt.out, std::ios::binary | std::ios::trunc);
    file << "P6\n" << opt.width << ' ' << opt.height << "\n255\n";
    file.write(rgb.data(), static_cast<std::streamsize>(rgb.size()));
    file.close();
    if (!file) {
        err << "protekt-render: could not write '" << opt.out << "'\n";
        return 1;
    }
    out << "wrote " << opt.out << " (" << opt.width << 'x' << opt.height << ", device "
        << renderer->device_name() << ", " << stats.draw_calls << " draw, " << stats.triangles
        << " triangles)\n";
    return 0;
}

} // namespace protekt::app

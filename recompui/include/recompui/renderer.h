#pragma once

#include <unordered_set>
#include <filesystem>

#include "common/rt64_user_configuration.h"
#include "ultramodern/renderer_context.hpp"
#include "librecomp/config.hpp"
#include "librecomp/mods.hpp"

namespace RT64 {
    struct Application;
}

namespace recompui {
    namespace renderer {
        inline const std::string special_option_texture_pack_enabled = "_recomp_texture_pack_enabled";

        class RT64Context final : public ultramodern::renderer::RendererContext {
        public:
            ~RT64Context() override;
            RT64Context(uint8_t *rdram, ultramodern::renderer::WindowHandle window_handle, ultramodern::renderer::PresentationMode presentation_mode, bool developer_mode);

            bool valid() override { return static_cast<bool>(app); }

            bool update_config(const ultramodern::renderer::GraphicsConfig &old_config, const ultramodern::renderer::GraphicsConfig &new_config) override;

            void enable_instant_present() override;
            void send_dl(const OSTask *task) override;
            void send_dummy_workload(uint32_t fb_address) override;
            void update_screen() override;
#ifdef __EMSCRIPTEN__
            // RT64 builds what the game's boot draws with before the game starts (RT64::Application::prepareForGame()).
            void prepare_for_game() override;
#endif
            void shutdown() override;
            uint32_t get_display_framerate() const override;
            float get_resolution_scale() const override;

        private:
            std::unique_ptr<RT64::Application> app;
            std::unordered_set<std::string> enabled_texture_packs;
            std::unordered_set<std::string> secondary_disabled_texture_packs;
            uint32_t last_refresh_rate = 0;

            void check_texture_pack_actions();
            void check_refresh_rate_changes();
        };

        std::unique_ptr<ultramodern::renderer::RendererContext> create_render_context(uint8_t *rdram, ultramodern::renderer::WindowHandle window_handle, ultramodern::renderer::PresentationMode presentation_mode, bool developer_mode);

        // Whether RT64 copies each frame it draws back into RDRAM (its render to RAM, on unless turned off), which a game
        // that reads its own frames on the CPU needs. A render context reads it when it is made. It costs a second draw
        // of the frame at the N64's resolution, and in a browser a wait of 5-6 ms per display list for the copy.
        void set_render_to_ram(bool enabled);

        // Whether that copy reaches RDRAM a display list late (RT64's renderToRAMLate, off unless turned on), so a list
        // doesn't wait for it. A game whose CPU reads a frame it has just drawn reads the one before then.
        void set_render_to_ram_late(bool late);

        RT64::UserConfiguration::Antialiasing RT64MaxMSAA();
        bool RT64SamplePositionsSupported();
        bool RT64HighPrecisionFBEnabled();

        void trigger_texture_pack_update();
        void enable_texture_pack(const recomp::mods::ModContext& context, const recomp::mods::ModHandle& mod);
        void disable_texture_pack(const recomp::mods::ModHandle& mod);
        void secondary_enable_texture_pack(const std::string& mod_id);
        void secondary_disable_texture_pack(const std::string& mod_id);

        // Texture pack enable option. Must be an enum with two options.
        // The first option is treated as disabled and the second option is treated as enabled.
        bool is_texture_pack_enable_config_option(const recomp::config::ConfigOption& option, bool show_errors);
    }
}

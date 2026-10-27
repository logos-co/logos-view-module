{
  description = "The view-module backend — plugin glue and CMake for Logos view (ui_qml) modules";

  # A LEAF by design. This repo emits the Qt plugin around a view's .rep and
  # *Backend, and builds the replica factory for the same .rep — neither of
  # which needs to read a LIDL contract, so nothing here depends on a generator
  # frontend, an SDK, or the protocol. Keeping it that way is what lets it be
  # re-pinned independently of the SDK stack.
  inputs = {
    logos-nix.url = "github:logos-co/logos-nix";
    nixpkgs.follows = "logos-nix/nixpkgs";
  };

  outputs = { self, nixpkgs, ... }:
    let
      systems = [ "aarch64-darwin" "x86_64-darwin" "aarch64-linux" "x86_64-linux" ];

      forAllSystems = f: nixpkgs.lib.genAttrs systems (system: f {
        inherit system;
        pkgs = import nixpkgs { inherit system; };
      });

      mkGenerator = pkgs: pkgs.stdenv.mkDerivation {
        pname = "logos-view-generator";
        version = "0.1.0";
        src = ./view-generator;
        nativeBuildInputs = [ pkgs.cmake pkgs.qt6.wrapQtAppsNoGuiHook ];
        buildInputs = [ pkgs.qt6.qtbase ];
      };

      # The CMake half: LogosViewModule.cmake plus the four .in templates it
      # resolves as siblings. They MUST stay in one directory — the function
      # locates them through CMAKE_CURRENT_FUNCTION_LIST_DIR.
      mkCmakeModule = pkgs: pkgs.runCommand "logos-view-module-cmake" {} ''
        mkdir -p $out/share/cmake/LogosViewModule
        cp ${./cmake}/LogosViewModule.cmake        $out/share/cmake/LogosViewModule/
        cp ${./cmake}/LogosViewPluginBase.h.in     $out/share/cmake/LogosViewModule/
        cp ${./cmake}/LogosViewPluginBase.cpp.in   $out/share/cmake/LogosViewModule/
        cp ${./cmake}/LogosViewReplicaFactory.h.in $out/share/cmake/LogosViewModule/
        cp ${./cmake}/LogosViewReplicaFactory.cpp.in $out/share/cmake/LogosViewModule/
      '';

      # The same four templates FLAT, as a nameable output. Two consumers want
      # them addressed as a bare directory rather than as part of an install
      # tree: LOGOS_VIEW_TEMPLATE_DIR, which logos-module-builder passes into
      # every ui_qml module build, and that repo's `view-interface-abi` check,
      # which reads ${dir}/LogosView*.h.in directly. Same bytes as
      # cmake-module's share/cmake/LogosViewModule/, one source.
      #
      # Deliberately NOT in the logos-view-module symlinkJoin: that join's
      # consumers expect share/ + include/ + bin/, and a second copy of the
      # .in files at its top level would be a second addressing scheme for
      # bytes that already have two.
      mkViewTemplates = pkgs: pkgs.runCommand "logos-view-templates" { } ''
        mkdir -p $out
        cp ${./cmake}/LogosView*.in $out/
      '';

      # The header a view's *Backend derives, alongside its repc SimpleSource.
      mkInclude = pkgs: pkgs.runCommand "logos-view-module-include" {} ''
        mkdir -p $out/include
        cp ${./cpp}/logos_ui_plugin_context.h $out/include/
        cp ${./cpp}/logos_view_channel.h $out/include/
      '';

    in {
      packages = forAllSystems ({ pkgs, ... }: rec {
        logos-view-generator = mkGenerator pkgs;
        cmake-module = mkCmakeModule pkgs;
        # Name is load-bearing: logos-module-builder looks this attribute up
        # by name (flake.nix, the view-interface-abi check).
        logos-view-templates = mkViewTemplates pkgs;
        include = mkInclude pkgs;
        logos-view-module = pkgs.symlinkJoin {
          name = "logos-view-module";
          paths = [ logos-view-generator cmake-module include ];
        };
        default = logos-view-module;
      });

      checks = forAllSystems ({ pkgs, system, ... }: {
        # Build a replica factory plugin from a .rep file, load it, and cast
        # it the way ui-host does. The binary-side half of the view ABI: the
        # `view-interface-abi` check in logos-module-builder compares the
        # module and host DECLARATIONS as text, and cannot see whether the
        # thing that comes out of a compiler still exports the IID or still
        # answers a qobject_cast.
        rep-file-plugin = import ./tests/test-rep-file-plugin.nix {
          inherit pkgs;
        };

        # The private channel a view backend run as a core module publishes
        # on: it must listen even under a macOS-length $TMPDIR.
        view-channel = import ./tests/test-view-channel.nix { inherit pkgs; };

        # Drive the generator over a real .rep + metadata.json and assert it
        # emits the three files with the class names scraped from them. Cheap,
        # and it is the only thing that catches a .rep parse regression.
        view-generator = pkgs.runCommand "logos-view-generator-test" {
          nativeBuildInputs = [ self.packages.${system}.logos-view-generator ];
        } ''
          mkdir -p work && cd work
          cat > metadata.json <<'EOF'
          { "name": "ticker_panel", "version": "2.1.0", "type": "ui_qml" }
          EOF
          cat > Ticker.rep <<'EOF'
          class TickerBackend
          {
              SLOT(void refresh());
              PROP(QString symbol READWRITE);
          };
          EOF
          logos-view-generator --metadata metadata.json --rep Ticker.rep --output-dir out

          for f in ticker_panel_ui_interface.h ticker_panel_ui_glue.h ticker_panel_ui_glue.cpp; do
            test -f "out/$f" || { echo "MISSING: $f"; exit 1; }
          done
          # The class stem is PascalCase of the module name, and the rep class
          # is scraped from the .rep — both are what a wrong parse gets wrong.
          grep -q "TickerPanel" out/ticker_panel_ui_glue.h \
            || { echo "plugin base class not derived from module name"; exit 1; }
          grep -q "TickerBackend" out/ticker_panel_ui_glue.cpp \
            || { echo "rep class not scraped from the .rep"; exit 1; }
          # version() is emitted inline in the glue HEADER, not the .cpp.
          grep -q 'version() const override.*"2.1.0"' out/ticker_panel_ui_glue.h \
            || { echo "version not carried from metadata.json"; exit 1; }

          # ── The teardown surface, as TEXT ────────────────────────────────
          # Carried from logos-qt-sdk's qtgen.ui_plugin_surface, which was the
          # only thing pinning these three properties before the emitter moved
          # here. Kept as the FAST signal; `ui-plugin-metaobject` is the one
          # that actually proves the host can reach them, because text in a
          # .cpp is not evidence that moc registered anything.
          #
          #   * the hook is INVOKABLE and returns int -- the host reads it back
          #     with Q_RETURN_ARG(int) and must not need the SDK enum;
          #   * the completion signal exists -- a view that answers
          #     Asynchronous with no way to say it is done gets refused the
          #     wait outright;
          #   * the emission is QUEUED -- the backend may finish on any thread,
          #     and the host is waiting on the plugin's.
          grep -q 'Q_INVOKABLE int aboutToUnload();' out/ticker_panel_ui_glue.h \
            || { echo "generated plugin does not declare Q_INVOKABLE int aboutToUnload()"; exit 1; }
          grep -q 'Q_SIGNALS:' out/ticker_panel_ui_glue.h \
            || { echo "generated plugin declares no Q_SIGNALS section"; exit 1; }
          grep -q 'void unloadFinished();' out/ticker_panel_ui_glue.h \
            || { echo "generated plugin does not declare the unloadFinished() signal"; exit 1; }
          grep -q 'maybeUiPluginAboutToUnload' out/ticker_panel_ui_glue.cpp \
            || { echo "aboutToUnload() does not delegate to the SFINAE helper"; exit 1; }
          grep -q 'Qt::QueuedConnection' out/ticker_panel_ui_glue.cpp \
            || { echo "unloadFinished() is not emitted through a QUEUED connection"; exit 1; }
          touch $out
        '';

        # The same surface, proven BEHAVIOURALLY: compile the emitted plugin,
        # load it with QPluginLoader, and drive the teardown handshake through
        # the meta-object the way ui-host does. See tests/ui-plugin-metaobject.
        ui-plugin-metaobject = import ./tests/test-ui-plugin-metaobject.nix {
          inherit pkgs;
          viewGenerator = self.packages.${system}.logos-view-generator;
        };

        # A missing/!unparseable .rep must FAIL, not emit half a plugin.
        view-generator-rejects-bad-rep = pkgs.runCommand "logos-view-generator-reject-test" {
          nativeBuildInputs = [ self.packages.${system}.logos-view-generator ];
        } ''
          mkdir -p work && cd work
          echo '{ "name": "broken", "version": "1.0.0" }' > metadata.json
          echo 'this file declares no class' > Bad.rep
          if logos-view-generator --metadata metadata.json --rep Bad.rep --output-dir out; then
            echo "generator accepted a .rep with no class"; exit 1
          fi
          touch $out
        '';
      });

      devShells = forAllSystems ({ pkgs, ... }: {
        default = pkgs.mkShell {
          packages = [ pkgs.cmake pkgs.ninja pkgs.qt6.qtbase pkgs.qt6.qtremoteobjects ];
        };
      });
    };
}

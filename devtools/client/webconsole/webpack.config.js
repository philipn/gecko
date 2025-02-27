/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

/* eslint-env node */
/* eslint max-len: [0] */

"use strict";

const {toolboxConfig} = require("./node_modules/devtools-launchpad/index");
const { NormalModuleReplacementPlugin } = require("webpack");
const {getConfig} = require("./bin/configure");

const path = require("path");
const projectPath = path.join(__dirname, "local-dev");

let webpackConfig = {
  entry: {
    console: [path.join(projectPath, "index.js")],
  },

  module: {
    rules: [
      {
        test: /\.(png|svg)$/,
        loader: "file-loader?name=[path][name].[ext]",
      },
      {
        /*
         * The version of webpack used in the launchpad seems to have trouble
         * with the require("raw!${file}") that we use for the properties
         * file in l10.js.
         * This loader goes through the whole code and remove the "raw!" prefix
         * so the raw-loader declared in devtools-launchpad config can load
         * those files.
         */
        test: /\.js/,
        loader: "rewrite-raw",
      },
    ]
  },

  resolveLoader: {
    modules: [
      path.resolve("./node_modules"),
      path.resolve("../shared/webpack"),
    ]
  },

  output: {
    path: path.join(__dirname, "assets/build"),
    filename: "[name].js",
    publicPath: "/assets/build",
  },

  externals: [
    {
      "promise": "var Promise",
    }
  ],
};

webpackConfig.resolve = {
  modules: [
    // Make sure webpack is always looking for modules in
    // `webconsole/node_modules` directory first.
    path.resolve(__dirname, "node_modules"), "node_modules"
  ],
  alias: {
    "Services": "devtools-modules/src/Services",

    "devtools/client/webconsole/jsterm": path.join(__dirname, "../../client/shared/webpack/shims/jsterm-stub"),
    "devtools/client/webconsole/utils": path.join(__dirname, "new-console-output/test/fixtures/WebConsoleUtils"),

    "devtools/client/shared/vendor/immutable": "immutable",
    "devtools/client/shared/vendor/react": "react",
    "devtools/client/shared/vendor/react-dom": "react-dom",
    "devtools/client/shared/vendor/react-redux": "react-redux",
    "devtools/client/shared/vendor/redux": "redux",
    "devtools/client/shared/vendor/reselect": "reselect",

    "devtools/shared/system": path.join(__dirname, "../../client/shared/webpack/shims/system-stub"),

    "devtools/client/framework/devtools": path.join(__dirname, "../../client/shared/webpack/shims/framework-devtools-shim"),
    "devtools/client/framework/menu": "devtools-modules/src/menu",
    "devtools/client/sourceeditor/editor": "devtools-source-editor/src/source-editor",

    "devtools/client/shared/zoom-keys": "devtools-modules/src/zoom-keys",

    "devtools/shared/fronts/timeline": path.join(__dirname, "../../client/shared/webpack/shims/fronts-timeline-shim"),
    "devtools/shared/old-event-emitter": "devtools-modules/src/utils/event-emitter",
    "devtools/shared/client/object-client": path.join(__dirname, "new-console-output/test/fixtures/ObjectClient"),
    "devtools/shared/platform/clipboard": path.join(__dirname, "../../client/shared/webpack/shims/platform-clipboard-stub"),
    "devtools/shared/platform/stack": path.join(__dirname, "../../client/shared/webpack/shims/platform-stack-stub"),

    // Locales need to be explicitly mapped to the en-US subfolder
    "toolkit/locales": path.join(__dirname, "../../../toolkit/locales/en-US"),
    "devtools/client/locales": path.join(__dirname, "../../client/locales/en-US"),
    "devtools/shared/locales": path.join(__dirname, "../../shared/locales/en-US"),
    "devtools/shim/locales": path.join(__dirname, "../../shared/locales/en-US"),

    // Unless a path explicitly needs to be rewritten or shimmed, all devtools paths can
    // be mapped to ../../
    "devtools": path.join(__dirname, "../../"),
  }
};

const mappings = [
  [
    /utils\/menu/, "devtools-launchpad/src/components/shared/menu"
  ],
  [
    /chrome:\/\/devtools\/skin/,
    (result) => {
      result.request = result.request
        .replace("./chrome://devtools/skin", path.join(__dirname, "../themes"));
    }
  ],
  [
    /chrome:\/\/devtools\/content/,
    (result) => {
      result.request = result.request
        .replace("./chrome://devtools/content", path.join(__dirname, ".."));
    }
  ],
  [
    /resource:\/\/devtools/,
    (result) => {
      result.request = result.request
        .replace("./resource://devtools/client", path.join(__dirname, ".."));
    }
  ],
];

webpackConfig.plugins = mappings.map(([regex, res]) =>
  new NormalModuleReplacementPlugin(regex, res));

const basePath = path.join(__dirname, "../../").replace(/\\/g, "\\\\");
const baseName = path.basename(__dirname);

let config = toolboxConfig(webpackConfig, getConfig(), {
  // Exclude to transpile all scripts in devtools/ but not for this folder
  babelExcludes: new RegExp(`^${basePath}(.(?!${baseName}))*$`)
});

// Remove loaders from devtools-launchpad's webpack.config.js
// * For svg-inline loader:
//   Webconsole uses file loader to bundle image assets instead of svg-inline-loader
config.module.rules = config.module.rules
  .filter((rule) => !["svg-inline-loader"].includes(rule.loader));

module.exports = config;

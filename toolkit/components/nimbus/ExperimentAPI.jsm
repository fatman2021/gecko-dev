/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

"use strict";

const EXPORTED_SYMBOLS = [
  "ExperimentAPI",
  "NimbusFeatures",
  "_ExperimentFeature",
];

const { XPCOMUtils } = ChromeUtils.importESModule(
  "resource://gre/modules/XPCOMUtils.sys.mjs"
);
const { AppConstants } = ChromeUtils.importESModule(
  "resource://gre/modules/AppConstants.sys.mjs"
);

const lazy = {};

ChromeUtils.defineESModuleGetters(lazy, {
  RemoteSettings: "resource://services-settings/remote-settings.sys.mjs",
});

XPCOMUtils.defineLazyModuleGetters(lazy, {
  ExperimentStore: "resource://nimbus/lib/ExperimentStore.jsm",
  ExperimentManager: "resource://nimbus/lib/ExperimentManager.jsm",
  FeatureManifest: "resource://nimbus/FeatureManifest.js",
});

const IS_MAIN_PROCESS =
  Services.appinfo.processType === Services.appinfo.PROCESS_TYPE_DEFAULT;

const COLLECTION_ID_PREF = "messaging-system.rsexperimentloader.collection_id";
const COLLECTION_ID_FALLBACK = "nimbus-desktop-experiments";
XPCOMUtils.defineLazyPreferenceGetter(
  lazy,
  "COLLECTION_ID",
  COLLECTION_ID_PREF,
  COLLECTION_ID_FALLBACK
);
const EXPOSURE_EVENT_CATEGORY = "normandy";
const EXPOSURE_EVENT_METHOD = "expose";
const EXPOSURE_EVENT_OBJECT = "nimbus_experiment";

function parseJSON(value) {
  if (value) {
    try {
      return JSON.parse(value);
    } catch (e) {
      console.error(e);
    }
  }
  return null;
}

function featuresCompat(branch) {
  if (!branch) {
    return [];
  }
  let { features } = branch;
  // In <=v1.5.0 of the Nimbus API, experiments had single feature
  if (!features) {
    features = [branch.feature];
  }

  return features;
}

const experimentBranchAccessor = {
  get: (target, prop) => {
    // Offer an API where we can access `branch.feature.*`.
    // This is a useful shorthand that hides the fact that
    // even single-feature recipes are still represented
    // as an array with 1 item
    if (!(prop in target) && target.features) {
      return target.features.find(f => f.featureId === prop);
    } else if (target.feature?.featureId === prop) {
      // Backwards compatibility for version 1.6.2 and older
      return target.feature;
    }

    return target[prop];
  },
};

const ExperimentAPI = {
  /**
   * @returns {Promise} Resolves when the API has synchronized to the main store
   */
  ready() {
    return this._store.ready();
  },

  /**
   * Returns an experiment, including all its metadata
   * Sends exposure event
   *
   * @param {{slug?: string, featureId?: string}} options slug = An experiment identifier
   * or feature = a stable identifier for a type of experiment
   * @returns {{slug: string, active: bool}} A matching experiment if one is found.
   */
  getExperiment({ slug, featureId } = {}) {
    if (!slug && !featureId) {
      throw new Error(
        "getExperiment(options) must include a slug or a feature."
      );
    }
    let experimentData;
    try {
      if (slug) {
        experimentData = this._store.get(slug);
      } else if (featureId) {
        experimentData = this._store.getExperimentForFeature(featureId);
      }
    } catch (e) {
      console.error(e);
    }
    if (experimentData) {
      return {
        slug: experimentData.slug,
        active: experimentData.active,
        branch: new Proxy(experimentData.branch, experimentBranchAccessor),
      };
    }

    return null;
  },

  /**
   * Used by getExperimentMetaData and getRolloutMetaData
   *
   * @param {{slug: string, featureId: string}} options Enrollment identifier
   * @param isRollout Is enrollment an experiment or a rollout
   * @returns {object} Enrollment metadata
   */
  getEnrollmentMetaData({ slug, featureId }, isRollout) {
    if (!slug && !featureId) {
      throw new Error(
        "getExperiment(options) must include a slug or a feature."
      );
    }

    let experimentData;
    try {
      if (slug) {
        experimentData = this._store.get(slug);
      } else if (featureId) {
        if (isRollout) {
          experimentData = this._store.getRolloutForFeature(featureId);
        } else {
          experimentData = this._store.getExperimentForFeature(featureId);
        }
      }
    } catch (e) {
      console.error(e);
    }
    if (experimentData) {
      return {
        slug: experimentData.slug,
        active: experimentData.active,
        branch: { slug: experimentData.branch.slug },
      };
    }

    return null;
  },

  /**
   * Return experiment slug its status and the enrolled branch slug
   * Does NOT send exposure event because you only have access to the slugs
   */
  getExperimentMetaData(options) {
    return this.getEnrollmentMetaData(options);
  },

  /**
   * Return rollout slug its status and the enrolled branch slug
   * Does NOT send exposure event because you only have access to the slugs
   */
  getRolloutMetaData(options) {
    return this.getEnrollmentMetaData(options, true);
  },

  /**
   * Return FeatureConfig from first active experiment where it can be found
   * @param {{slug: string, featureId: string }}
   * @returns {Branch | null}
   */
  getActiveBranch({ slug, featureId }) {
    let experiment = null;
    try {
      if (slug) {
        experiment = this._store.get(slug);
      } else if (featureId) {
        experiment = this._store.getExperimentForFeature(featureId);
      }
    } catch (e) {
      console.error(e);
    }

    if (!experiment) {
      return null;
    }

    // Default to null for feature-less experiments where we're only
    // interested in exposure.
    return experiment?.branch || null;
  },

  /**
   * Deregisters an event listener.
   * @param {string} eventName
   * @param {function} callback
   */
  off(eventName, callback) {
    this._store.off(eventName, callback);
  },

  /**
   * Returns the recipe for a given experiment slug
   *
   * This should noly be called from the main process.
   *
   * Note that the recipe is directly fetched from RemoteSettings, which has
   * all the recipe metadata available without relying on the `this._store`.
   * Therefore, calling this function does not require to call `this.ready()` first.
   *
   * @param slug {String} An experiment identifier
   * @returns {Recipe|undefined} A matching experiment recipe if one is found
   */
  async getRecipe(slug) {
    if (!IS_MAIN_PROCESS) {
      throw new Error(
        "getRecipe() should only be called from the main process"
      );
    }

    let recipe;

    try {
      [recipe] = await this._remoteSettingsClient.get({
        // Do not sync the RS store, let RemoteSettingsExperimentLoader do that
        syncIfEmpty: false,
        filters: { slug },
      });
    } catch (e) {
      // If an error occurs in .get(), an empty list is returned and the destructuring
      // assignment will throw.
      console.error(e);
      recipe = undefined;
    }

    return recipe;
  },

  /**
   * Returns all the branches for a given experiment slug
   *
   * This should only be called from the main process. Like `getRecipe()`,
   * calling this function does not require to call `this.ready()` first.
   *
   * @param slug {String} An experiment identifier
   * @returns {[Branches]|undefined} An array of branches for the given slug
   */
  async getAllBranches(slug) {
    if (!IS_MAIN_PROCESS) {
      throw new Error(
        "getAllBranches() should only be called from the main process"
      );
    }

    const recipe = await this.getRecipe(slug);
    return recipe?.branches.map(
      branch => new Proxy(branch, experimentBranchAccessor)
    );
  },

  recordExposureEvent({ featureId, experimentSlug, branchSlug }) {
    Services.telemetry.setEventRecordingEnabled(EXPOSURE_EVENT_CATEGORY, true);
    try {
      Services.telemetry.recordEvent(
        EXPOSURE_EVENT_CATEGORY,
        EXPOSURE_EVENT_METHOD,
        EXPOSURE_EVENT_OBJECT,
        experimentSlug,
        {
          branchSlug,
          featureId,
        }
      );
    } catch (e) {
      console.error(e);
    }
    Glean.nimbusEvents.exposure.record({
      experiment: experimentSlug,
      branch: branchSlug,
      feature_id: featureId,
    });
  },
};

/**
 * Singleton that holds lazy references to _ExperimentFeature instances
 * defined by the FeatureManifest
 */
const NimbusFeatures = {};
for (let feature in lazy.FeatureManifest) {
  XPCOMUtils.defineLazyGetter(NimbusFeatures, feature, () => {
    return new _ExperimentFeature(feature);
  });
}

class _ExperimentFeature {
  constructor(featureId, manifest) {
    this.featureId = featureId;
    this.prefGetters = {};
    this.manifest = manifest || lazy.FeatureManifest[featureId];
    if (!this.manifest) {
      console.error(
        `No manifest entry for ${featureId}. Please add one to toolkit/components/nimbus/FeatureManifest.js`
      );
    }
    this._didSendExposureEvent = false;
    const variables = this.manifest?.variables || {};

    Object.keys(variables).forEach(key => {
      const { type, fallbackPref } = variables[key];
      if (fallbackPref) {
        XPCOMUtils.defineLazyPreferenceGetter(
          this.prefGetters,
          key,
          fallbackPref,
          null,
          () => {
            ExperimentAPI._store._emitFeatureUpdate(
              this.featureId,
              "pref-updated"
            );
          },
          type === "json" ? parseJSON : val => val
        );
      }
    });
  }

  getSetPrefName(variable) {
    return this.manifest?.variables?.[variable]?.setPref;
  }

  getFallbackPrefName(variable) {
    return this.manifest?.variables?.[variable]?.fallbackPref;
  }

  /**
   * Wait for ExperimentStore to load giving access to experiment features that
   * do not have a pref cache
   */
  ready() {
    return ExperimentAPI.ready();
  }

  /**
   * Lookup feature variables in experiments, prefs, and remote defaults.
   * @param {{defaultValues?: {[variableName: string]: any}}} options
   * @returns {{[variableName: string]: any}} The feature value
   */
  getAllVariables({ defaultValues = null } = {}) {
    const branch = ExperimentAPI.getActiveBranch({ featureId: this.featureId });
    const featureValue = featuresCompat(branch).find(
      ({ featureId }) => featureId === this.featureId
    )?.value;

    return {
      ...this.prefGetters,
      ...defaultValues,
      ...(featureValue ? featureValue : this.getRollout()?.value),
    };
  }

  getVariable(variable) {
    if (!this.manifest?.variables?.[variable]) {
      // Only throw in nightly/tests
      if (Cu.isInAutomation || AppConstants.NIGHTLY_BUILD) {
        throw new Error(
          `Nimbus: Warning - variable "${variable}" is not defined in FeatureManifest.js`
        );
      }
    }

    // Next, check if an experiment is defined
    const branch = ExperimentAPI.getActiveBranch({
      featureId: this.featureId,
    });
    const experimentValue = featuresCompat(branch).find(
      ({ featureId }) => featureId === this.featureId
    )?.value?.[variable];

    if (typeof experimentValue !== "undefined") {
      return experimentValue;
    }

    // Next, check remote defaults
    const remoteValue = this.getRollout()?.value?.[variable];
    if (typeof remoteValue !== "undefined") {
      return remoteValue;
    }

    // Return the default preference value
    const prefName = this.getFallbackPrefName(variable);
    return prefName ? this.prefGetters[variable] : undefined;
  }

  getRollout() {
    let remoteConfig = ExperimentAPI._store.getRolloutForFeature(
      this.featureId
    );
    if (!remoteConfig) {
      return null;
    }

    if (remoteConfig.branch?.features) {
      return remoteConfig.branch?.features.find(
        f => f.featureId === this.featureId
      );
    }

    // This path is deprecated and will be removed in the future
    if (remoteConfig.branch?.feature) {
      return remoteConfig.branch.feature;
    }

    return null;
  }

  recordExposureEvent({ once = false } = {}) {
    if (once && this._didSendExposureEvent) {
      return;
    }

    let enrollmentData = ExperimentAPI.getExperimentMetaData({
      featureId: this.featureId,
    });
    if (!enrollmentData) {
      enrollmentData = ExperimentAPI.getRolloutMetaData({
        featureId: this.featureId,
      });
    }

    // Exposure only sent if user is enrolled in an experiment
    if (enrollmentData) {
      ExperimentAPI.recordExposureEvent({
        featureId: this.featureId,
        experimentSlug: enrollmentData.slug,
        branchSlug: enrollmentData.branch?.slug,
      });
      this._didSendExposureEvent = true;
    }
  }

  onUpdate(callback) {
    ExperimentAPI._store._onFeatureUpdate(this.featureId, callback);
  }

  offUpdate(callback) {
    ExperimentAPI._store._offFeatureUpdate(this.featureId, callback);
  }

  /**
   * The applications this feature applies to.
   *
   */
  get applications() {
    return this.manifest.applications ?? ["firefox-desktop"];
  }

  debug() {
    return {
      variables: this.getAllVariables(),
      experiment: ExperimentAPI.getExperimentMetaData({
        featureId: this.featureId,
      }),
      fallbackPrefs: Object.keys(this.prefGetters).map(prefName => [
        prefName,
        this.prefGetters[prefName],
      ]),
      rollouts: this.getRollout(),
    };
  }
}

XPCOMUtils.defineLazyGetter(ExperimentAPI, "_store", function() {
  return IS_MAIN_PROCESS
    ? lazy.ExperimentManager.store
    : new lazy.ExperimentStore();
});

XPCOMUtils.defineLazyGetter(ExperimentAPI, "_remoteSettingsClient", function() {
  return lazy.RemoteSettings(lazy.COLLECTION_ID);
});

const TEST_DOMAIN = "https://example.com";
const TEST_DOMAIN_ANOTHER = "https://example.org";
const TEST_DOMAIN_THIRD = "https://example.net";

/**
 * A helper function to get the random key in a hex string format and test if
 * the random key works properly.
 *
 * @param {Browser} browser The browser element of the testing tab.
 * @param {String} firstPartyDomain The first-party domain loaded on the tab
 * @param {String} thirdPartyDomain The third-party domain to test
 * @returns {String} The random key hex string
 */
async function getRandomKeyHexFromBrowser(
  browser,
  firstPartyDomain,
  thirdPartyDomain
) {
  // Get the key from the cookieJarSettings of the browser element.
  let key = browser.cookieJarSettings.fingerprintingRandomizationKey;
  let keyHex = key.map(bytes => bytes.toString(16).padStart(2, "0")).join("");

  // Get the key from the cookieJarSettings of the top-level document.
  let keyTop = await SpecialPowers.spawn(browser, [], _ => {
    return content.document.cookieJarSettings.fingerprintingRandomizationKey;
  });
  let keyTopHex = keyTop
    .map(bytes => bytes.toString(16).padStart(2, "0"))
    .join("");

  is(
    keyTopHex,
    keyHex,
    "The fingerprinting random key should match between the browser element and the top-level document."
  );

  // Get the key from the cookieJarSettings of an about:blank iframe.
  let keyAboutBlank = await SpecialPowers.spawn(browser, [], async _ => {
    let ifr = content.document.createElement("iframe");

    let loaded = new content.Promise(resolve => {
      ifr.onload = resolve;
    });
    content.document.body.appendChild(ifr);
    ifr.src = "about:blank";

    await loaded;

    return SpecialPowers.spawn(ifr, [], _ => {
      return content.document.cookieJarSettings.fingerprintingRandomizationKey;
    });
  });

  let keyAboutBlankHex = keyAboutBlank
    .map(bytes => bytes.toString(16).padStart(2, "0"))
    .join("");
  is(
    keyAboutBlankHex,
    keyHex,
    "The fingerprinting random key should match between the browser element and the about:blank iframe document."
  );

  // Get the key from the cookieJarSettings of an first-party iframe.
  let keyFirstPartyFrame = await SpecialPowers.spawn(
    browser,
    [firstPartyDomain],
    async domain => {
      let ifr = content.document.createElement("iframe");

      let loaded = new content.Promise(resolve => {
        ifr.onload = resolve;
      });
      content.document.body.appendChild(ifr);
      ifr.src = domain;

      await loaded;

      return SpecialPowers.spawn(ifr, [], _ => {
        return content.document.cookieJarSettings
          .fingerprintingRandomizationKey;
      });
    }
  );

  let keyFirstPartyFrameHex = keyFirstPartyFrame
    .map(bytes => bytes.toString(16).padStart(2, "0"))
    .join("");
  is(
    keyFirstPartyFrameHex,
    keyHex,
    "The fingerprinting random key should match between the browser element and the first-party iframe document."
  );

  // Get the key from the cookieJarSettings of an third-party iframe
  let keyThirdPartyFrame = await SpecialPowers.spawn(
    browser,
    [thirdPartyDomain],
    async domain => {
      let ifr = content.document.createElement("iframe");

      let loaded = new content.Promise(resolve => {
        ifr.onload = resolve;
      });
      content.document.body.appendChild(ifr);
      ifr.src = domain;

      await loaded;

      return SpecialPowers.spawn(ifr, [], _ => {
        return content.document.cookieJarSettings
          .fingerprintingRandomizationKey;
      });
    }
  );

  let keyThirdPartyFrameHex = keyThirdPartyFrame
    .map(bytes => bytes.toString(16).padStart(2, "0"))
    .join("");
  is(
    keyThirdPartyFrameHex,
    keyHex,
    "The fingerprinting random key should match between the browser element and the third-party iframe document."
  );

  return keyHex;
}

// Test accessing the fingerprinting randomization key will throw if
// fingerprinting randomization is disabled.
add_task(async function test_randomization_disabled() {
  await SpecialPowers.pushPrefEnv({
    set: [["privacy.resistFingerprinting.randomization.enabled", false]],
  });

  // Ensure accessing the fingerprinting randomization key of the browser
  // element will throw if fingerprinting randomization is disabled.
  let tab = await BrowserTestUtils.openNewForegroundTab(gBrowser, TEST_DOMAIN);

  try {
    let key =
      tab.linkedBrowser.cookieJarSettings.fingerprintingRandomizationKey;
    ok(
      false,
      `Accessing the fingerprinting randomization key should throw when randomization is disabled. ${key}`
    );
  } catch (e) {
    ok(
      true,
      "It should throw when getting the key when randomization is disabled."
    );
  }

  // Ensure accessing the fingerprinting randomization key of the top-level
  // document will throw if fingerprinting randomization is disabled.
  try {
    await SpecialPowers.spawn(tab.linkedBrowser, [], _ => {
      return content.document.cookieJarSettings.fingerprintingRandomizationKey;
    });
  } catch (e) {
    ok(
      true,
      "It should throw when getting the key when randomization is disabled."
    );
  }

  BrowserTestUtils.removeTab(tab);
});

// Test accessing the fingerprinting randomization key will throw if
// fingerprinting resistance is disabled.
add_task(async function test_randomization_disabled_with_rfp_disabled() {
  await SpecialPowers.pushPrefEnv({
    set: [
      ["privacy.resistFingerprinting.randomization.enabled", true],
      ["privacy.resistFingerprinting", false],
    ],
  });

  // Ensure accessing the fingerprinting randomization key of the browser
  // element will throw if fingerprinting randomization is disabled.
  let tab = await BrowserTestUtils.openNewForegroundTab(gBrowser, TEST_DOMAIN);

  try {
    let key =
      tab.linkedBrowser.cookieJarSettings.fingerprintingRandomizationKey;
    ok(
      false,
      `Accessing the fingerprinting randomization key should throw when fingerprinting resistance is disabled. ${key}`
    );
  } catch (e) {
    ok(
      true,
      "It should throw when getting the key when fingerprinting resistance is disabled."
    );
  }

  // Ensure accessing the fingerprinting randomization key of the top-level
  // document will throw if fingerprinting randomization is disabled.
  try {
    await SpecialPowers.spawn(tab.linkedBrowser, [], _ => {
      return content.document.cookieJarSettings.fingerprintingRandomizationKey;
    });
  } catch (e) {
    ok(
      true,
      "It should throw when getting the key when fingerprinting resistance is disabled."
    );
  }

  BrowserTestUtils.removeTab(tab);
});

// Test the fingerprinting randomization key generation.
add_task(async function test_generate_randomization_key() {
  await SpecialPowers.pushPrefEnv({
    set: [
      ["privacy.resistFingerprinting.randomization.enabled", true],
      ["privacy.resistFingerprinting", true],
    ],
  });

  for (let testPrivateWin of [true, false]) {
    let win = window;

    if (testPrivateWin) {
      win = await BrowserTestUtils.openNewBrowserWindow({
        private: true,
      });
    }

    let tabOne = await BrowserTestUtils.openNewForegroundTab(
      win.gBrowser,
      TEST_DOMAIN
    );
    let keyHexOne;

    try {
      keyHexOne = await getRandomKeyHexFromBrowser(
        tabOne.linkedBrowser,
        TEST_DOMAIN,
        TEST_DOMAIN_THIRD
      );
      ok(true, `The fingerprinting random key: ${keyHexOne}`);
    } catch (e) {
      ok(
        false,
        "Shouldn't fail when getting the random key from the cookieJarSettings"
      );
    }

    // Open the test domain again and check if the key remains the same.
    let tabTwo = await BrowserTestUtils.openNewForegroundTab(
      win.gBrowser,
      TEST_DOMAIN
    );
    try {
      let keyHexTwo = await getRandomKeyHexFromBrowser(
        tabTwo.linkedBrowser,
        TEST_DOMAIN,
        TEST_DOMAIN_THIRD
      );
      is(
        keyHexTwo,
        keyHexOne,
        `The key should remain the same after reopening the tab.`
      );
    } catch (e) {
      ok(
        false,
        "Shouldn't fail when getting the random key from the cookieJarSettings"
      );
    }

    // Open a tab with a different domain to see if the key changes.
    let tabAnother = await BrowserTestUtils.openNewForegroundTab(
      win.gBrowser,
      TEST_DOMAIN_ANOTHER
    );
    try {
      let keyHexAnother = await getRandomKeyHexFromBrowser(
        tabAnother.linkedBrowser,
        TEST_DOMAIN_ANOTHER,
        TEST_DOMAIN_THIRD
      );
      isnot(
        keyHexAnother,
        keyHexOne,
        `The key should be different when loading a different domain`
      );
    } catch (e) {
      ok(
        false,
        "Shouldn't fail when getting the random key from the cookieJarSettings"
      );
    }

    BrowserTestUtils.removeTab(tabOne);
    BrowserTestUtils.removeTab(tabTwo);
    BrowserTestUtils.removeTab(tabAnother);
    if (testPrivateWin) {
      await BrowserTestUtils.closeWindow(win);
    }
  }
});

// Test the fingerprinting randomization key will change after private session
// ends.
add_task(async function test_reset_key_after_pbm_session_ends() {
  await SpecialPowers.pushPrefEnv({
    set: [
      ["privacy.resistFingerprinting.randomization.enabled", true],
      ["privacy.resistFingerprinting", true],
    ],
  });

  let privateWin = await BrowserTestUtils.openNewBrowserWindow({
    private: true,
  });

  // Open a tab in the private window.
  let tab = await BrowserTestUtils.openNewForegroundTab(
    privateWin.gBrowser,
    TEST_DOMAIN
  );

  let keyHex = await getRandomKeyHexFromBrowser(
    tab.linkedBrowser,
    TEST_DOMAIN,
    TEST_DOMAIN_THIRD
  );

  // Close the window and open another private window.
  BrowserTestUtils.removeTab(tab);
  await BrowserTestUtils.closeWindow(privateWin);

  privateWin = await BrowserTestUtils.openNewBrowserWindow({
    private: true,
  });

  // Open a tab again in the new private window.
  tab = await BrowserTestUtils.openNewForegroundTab(
    privateWin.gBrowser,
    TEST_DOMAIN
  );

  let keyHexNew = await getRandomKeyHexFromBrowser(
    tab.linkedBrowser,
    TEST_DOMAIN,
    TEST_DOMAIN_THIRD
  );

  // Ensure the keys are different.
  isnot(keyHexNew, keyHex, "Ensure the new key is different from the old one.");

  BrowserTestUtils.removeTab(tab);
  await BrowserTestUtils.closeWindow(privateWin);
});

// Test accessing the fingerprinting randomization key will throw in normal
// windows if we exempt fingerprinting protection in normal windows.
add_task(async function test_randomization_with_exempted_normal_window() {
  await SpecialPowers.pushPrefEnv({
    set: [
      ["privacy.resistFingerprinting.randomization.enabled", true],
      ["privacy.resistFingerprinting", true],
      ["privacy.resistFingerprinting.testGranularityMask", 2],
    ],
  });

  // Ensure accessing the fingerprinting randomization key of the browser
  // element will throw if fingerprinting randomization is exempted from normal
  // windows.
  let tab = await BrowserTestUtils.openNewForegroundTab(gBrowser, TEST_DOMAIN);

  try {
    let key =
      tab.linkedBrowser.cookieJarSettings.fingerprintingRandomizationKey;
    ok(
      false,
      `Accessing the fingerprinting randomization key should throw when fingerprinting resistance is exempted in normal windows. ${key}`
    );
  } catch (e) {
    ok(
      true,
      "It should throw when getting the key when fingerprinting resistance is exempted in normal windows."
    );
  }

  // Ensure accessing the fingerprinting randomization key of the top-level
  // document will throw if fingerprinting randomization is exempted from normal
  // windows.
  try {
    await SpecialPowers.spawn(tab.linkedBrowser, [], _ => {
      return content.document.cookieJarSettings.fingerprintingRandomizationKey;
    });
  } catch (e) {
    ok(
      true,
      "It should throw when getting the key when fingerprinting resistance is exempted in normal windows."
    );
  }

  BrowserTestUtils.removeTab(tab);

  // Open a private window and check the key can be accessed there.
  let privateWin = await BrowserTestUtils.openNewBrowserWindow({
    private: true,
  });

  // Open a tab in the private window.
  tab = await BrowserTestUtils.openNewForegroundTab(
    privateWin.gBrowser,
    TEST_DOMAIN
  );

  // Access the key, this shouldn't throw an error.
  await getRandomKeyHexFromBrowser(
    tab.linkedBrowser,
    TEST_DOMAIN,
    TEST_DOMAIN_THIRD
  );

  BrowserTestUtils.removeTab(tab);
  await BrowserTestUtils.closeWindow(privateWin);
});

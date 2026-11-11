import { test, expect } from '@playwright/test';

test.skip(!process.env.GLAMDB_NATIVE_SERVER, 'native server profile only');
test('native create append scan and unsupported metadata views', async ({ page }) => {
    const errors = [];
    page.on('pageerror', (error) => errors.push(error.message));
    await page.goto('/');
    await page.getByLabel('Access token').fill('e2e-only-local-token');
    await page.getByRole('button', { name: 'Open my workspace' }).click();
    await page.getByRole('button', { name: 'New collection', exact: true }).first().click();
    await page.getByLabel('Collection name').fill('native_browser');
    await page.getByRole('button', { name: 'Create collection' }).click();
    await expect(page.getByRole('dialog')).not.toBeVisible();
    await expect(page.locator('.breadcrumb strong')).toHaveText('native_browser');
    await page.locator('nav').getByRole('button', { name: 'Data studio', exact: true }).click();
    await page.getByRole('button', { name: 'Append rows', exact: true }).click();
    const dialog = page.getByRole('dialog');
    await dialog
        .getByLabel('Rows · JSON array')
        .fill('[{"key":"persisted-browser-row","label":"native","amount":"9223372036854775807"}]');
    await dialog.getByRole('button', { name: 'Append rows', exact: true }).click();
    await expect(dialog.getByText('Append committed', { exact: true })).toBeVisible();
    await dialog.getByRole('button', { name: 'Close dialog' }).click();
    await page.locator('#run-query').click();
    await expect(page.getByText('persisted-browser-row', { exact: true })).toBeVisible();
    for (const name of ['Snapshots', 'Storage', 'Explain']) {
        await page.locator('nav').getByRole('button', { name, exact: true }).click();
        await expect(
            page.getByText('This server does not yet expose this capability.'),
        ).toBeVisible();
    }
    expect(errors).toEqual([]);
});

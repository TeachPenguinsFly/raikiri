import assert from 'node:assert/strict'
import { test } from 'node:test'
import { isNewer, keepNewest, LatestOnly } from './latest.ts'

test('a slow poll landing after a later one is dropped', () => {
  const polls = new LatestOnly()
  const slow = polls.begin()
  const fast = polls.begin()
  assert.equal(polls.accept(fast), true)
  assert.equal(polls.accept(slow), false)
})

test('polls landing in order all count, and a repeat of the newest is harmless', () => {
  const polls = new LatestOnly()
  const first = polls.begin()
  const second = polls.begin()
  assert.equal(polls.accept(first), true)
  assert.equal(polls.accept(second), true)
  assert.equal(polls.accept(second), true)
})

test('versions compare as numbers, past what a double holds', () => {
  assert.equal(isNewer('1762041600000000002', '1762041600000000001'), true)
  assert.equal(isNewer('1762041600000000001', '1762041600000000002'), false)
  assert.equal(isNewer('10', '9'), true)
  assert.equal(isNewer('9', '10'), false)
  assert.equal(isNewer('7', '7'), false)
  assert.equal(isNewer(undefined, '7'), false)
  assert.equal(isNewer('7', undefined), false)
})

test('a poll older than what a stream frame already showed does not take the row back', () => {
  const held = [{ id: 'a', status: 'filled', version: '12' }]
  const poll = [{ id: 'a', status: 'accepted', version: '11' }]
  assert.deepEqual(keepNewest(held, poll, (r) => r.id), held)
})

test('a newer row replaces, a new row appears, and a row the answer dropped goes', () => {
  const held = [
    { id: 'a', version: '1' },
    { id: 'gone', version: '9' },
  ]
  const next = [
    { id: 'a', version: '2' },
    { id: 'new', version: '1' },
  ]
  assert.deepEqual(keepNewest(held, next, (r) => r.id), next)
})

test('rows without a version take the answer', () => {
  const held = [{ id: 'a', n: 1 }]
  const next = [{ id: 'a', n: 2 }]
  assert.deepEqual(keepNewest(held, next, (r) => r.id), next)
})
